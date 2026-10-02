#include "linux_vulkan.h"
#if !defined(_WIN32)
#include "linux_gpu_share.h"
#include "perf_stats.h"
#include <openxr/openxr_platform.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace axrb::host {
namespace {
bool ok(VkResult result, const char* what)
{
    if (result == VK_SUCCESS) return true;
    std::fprintf(stderr, "AXRB Vulkan: %s failed: %d\n", what, result);
    return false;
}
constexpr VkImageUsageFlags kHostImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
} // namespace

VulkanContext::~VulkanContext()
{
    if (device && vkDeviceWaitIdle) vkDeviceWaitIdle(device);
    if (device && vkDestroyDevice) vkDestroyDevice(device, nullptr);
    if (instance && vkDestroyInstance) vkDestroyInstance(instance, nullptr);
    if (library) dlclose(library);
}

uint32_t VulkanContext::memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const
{
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    return UINT32_MAX;
}

HostImage::~HostImage()
{
    if (!vk) return;
    if (image) vk->vkDestroyImage(vk->device, image, nullptr);
    if (memory) vk->vkFreeMemory(vk->device, memory, nullptr);
}

LinuxVulkan::~LinuxVulkan()
{
    if (shareSocket_ >= 0) close(shareSocket_);
    if (!vk_ || !vk_->device) return;
    vk_->vkDeviceWaitIdle(vk_->device);
    for (auto& import : imports_) destroy(import);
    if (staging_) vk_->vkDestroyBuffer(vk_->device, staging_, nullptr);
    if (stagingMemory_) vk_->vkFreeMemory(vk_->device, stagingMemory_, nullptr);
    if (capture_) vk_->vkDestroyBuffer(vk_->device, capture_, nullptr);
    if (captureMemory_) vk_->vkFreeMemory(vk_->device, captureMemory_, nullptr);
    for (auto* commands : {&present_, &receive_}) {
        if (commands->fence) vk_->vkDestroyFence(vk_->device, commands->fence, nullptr);
        if (commands->pool) vk_->vkDestroyCommandPool(vk_->device, commands->pool, nullptr);
    }
}

bool LinuxVulkan::create(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr getProcAddr)
{
    auto load = [&](const char* name, auto* out) {
        PFN_xrVoidFunction function = nullptr;
        if (getProcAddr(instance, name, &function) != XR_SUCCESS || !function) {
            std::fprintf(stderr, "AXRB Vulkan: runtime lacks %s\n", name);
            return false;
        }
        *out = reinterpret_cast<std::remove_pointer_t<decltype(out)>>(function);
        return true;
    };
    PFN_xrGetVulkanGraphicsRequirements2KHR requirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR graphicsDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createDevice = nullptr;
    if (!load("xrGetVulkanGraphicsRequirements2KHR", &requirements) ||
        !load("xrCreateVulkanInstanceKHR", &createInstance) ||
        !load("xrGetVulkanGraphicsDevice2KHR", &graphicsDevice) ||
        !load("xrCreateVulkanDeviceKHR", &createDevice)) return false;

    vk_ = std::make_shared<VulkanContext>();
    auto& vk = *vk_;
    vk.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!vk.library) { std::fprintf(stderr, "AXRB Vulkan: cannot load libvulkan.so.1\n"); return false; }
    vk.vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vk.library, "vkGetInstanceProcAddr"));
    if (!vk.vkGetInstanceProcAddr) return false;

    XrGraphicsRequirementsVulkan2KHR graphicsRequirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (requirements(instance, system, &graphicsRequirements) != XR_SUCCESS) return false;

    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "AXRB host bridge";
    application.pEngineName = "AXRB";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &application;
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = system;
    xrInstanceInfo.pfnGetInstanceProcAddr = vk.vkGetInstanceProcAddr;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vkResult = VK_SUCCESS;
    if (createInstance(instance, &xrInstanceInfo, &vk.instance, &vkResult) != XR_SUCCESS || !ok(vkResult, "vkCreateInstance"))
        return false;
#define AXRB_LOAD(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetInstanceProcAddr(vk.instance, #name)); \
    if (!vk.name) { std::fprintf(stderr, "AXRB Vulkan: missing %s\n", #name); return false; }
    AXRB_VK_INSTANCE_FUNCTIONS(AXRB_LOAD)
#undef AXRB_LOAD

    XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGetInfo.systemId = system;
    deviceGetInfo.vulkanInstance = vk.instance;
    if (graphicsDevice(instance, &deviceGetInfo, &vk.physical) != XR_SUCCESS || !vk.physical) return false;
    vk.vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &id;
    vk.vkGetPhysicalDeviceProperties2(vk.physical, &properties);
    std::memcpy(vk.deviceUUID, id.deviceUUID, VK_UUID_SIZE);
    std::memcpy(vk.driverUUID, id.driverUUID, VK_UUID_SIZE);

    uint32_t familyCount = 0;
    vk.vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vk.vkGetPhysicalDeviceQueueFamilyProperties(vk.physical, &familyCount, families.data());
    vk.family = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i)
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { vk.family = i; break; }
    if (vk.family == UINT32_MAX) return false;
    // A second queue lets reception copy while the frame loop owns queue 0,
    // which the runtime may also use inside xrEndFrame and swapchain calls.
    const uint32_t queueCount = std::min<uint32_t>(2, families[vk.family].queueCount);
    sharedQueue_ = queueCount < 2;
    const float priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = vk.family;
    queueInfo.queueCount = queueCount;
    queueInfo.pQueuePriorities = priorities;
    const char* extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = 1;
    deviceInfo.ppEnabledExtensionNames = extensions;
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = system;
    xrDeviceInfo.pfnGetInstanceProcAddr = vk.vkGetInstanceProcAddr;
    xrDeviceInfo.vulkanPhysicalDevice = vk.physical;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    if (createDevice(instance, &xrDeviceInfo, &vk.device, &vkResult) != XR_SUCCESS || !ok(vkResult, "vkCreateDevice"))
        return false;
#define AXRB_LOAD(name) vk.name = reinterpret_cast<PFN_##name>(vk.vkGetDeviceProcAddr(vk.device, #name)); \
    if (!vk.name) { std::fprintf(stderr, "AXRB Vulkan: missing %s\n", #name); return false; }
    AXRB_VK_DEVICE_FUNCTIONS(AXRB_LOAD)
#undef AXRB_LOAD
    if (!create_commands(present_, 0) || !create_commands(receive_, sharedQueue_ ? 0 : 1)) return false;

    binding_.instance = vk.instance;
    binding_.physicalDevice = vk.physical;
    binding_.device = vk.device;
    binding_.queueFamilyIndex = vk.family;
    binding_.queueIndex = 0;
    std::fprintf(stderr, "AXRB Vulkan: %s bound to the OpenXR runtime (family %u, %s)\n",
        properties.properties.deviceName, vk.family, sharedQueue_ ? "one shared queue" : "separate receive queue");
    return true;
}

bool LinuxVulkan::create_commands(Commands& commands, uint32_t queueIndex)
{
    auto& vk = *vk_;
    vk.vkGetDeviceQueue(vk.device, vk.family, queueIndex, &commands.queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = vk.family;
    if (!ok(vk.vkCreateCommandPool(vk.device, &pool, nullptr, &commands.pool), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = commands.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    return ok(vk.vkAllocateCommandBuffers(vk.device, &allocate, &commands.cmd), "vkAllocateCommandBuffers") &&
        ok(vk.vkCreateFence(vk.device, &fence, nullptr, &commands.fence), "vkCreateFence");
}

bool LinuxVulkan::begin(Commands& commands)
{
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return ok(vk_->vkResetCommandBuffer(commands.cmd, 0), "vkResetCommandBuffer") &&
        ok(vk_->vkBeginCommandBuffer(commands.cmd, &info), "vkBeginCommandBuffer");
}

bool LinuxVulkan::submit_and_wait(Commands& commands)
{
    auto& vk = *vk_;
    if (!ok(vk.vkEndCommandBuffer(commands.cmd), "vkEndCommandBuffer") ||
        !ok(vk.vkResetFences(vk.device, 1, &commands.fence), "vkResetFences")) return false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &commands.cmd;
    {
        std::unique_lock lock(sharedQueueMutex_, std::defer_lock);
        if (sharedQueue_) lock.lock();
        if (!ok(vk.vkQueueSubmit(commands.queue, 1, &submit, commands.fence), "vkQueueSubmit")) return false;
    }
    return ok(vk.vkWaitForFences(vk.device, 1, &commands.fence, VK_TRUE, 1'000'000'000ull), "vkWaitForFences");
}

void LinuxVulkan::barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers, VkImageLayout before, VkImageLayout after,
                          VkAccessFlags src, VkAccessFlags dst, uint32_t srcFamily, uint32_t dstFamily)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = before; b.newLayout = after; b.srcAccessMask = src; b.dstAccessMask = dst;
    b.srcQueueFamilyIndex = srcFamily; b.dstQueueFamilyIndex = dstFamily;
    b.image = image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vk_->vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &b);
}

void LinuxVulkan::destroy(Import& import)
{
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (import.images[eye]) vk_->vkDestroyImage(vk_->device, import.images[eye], nullptr);
        if (import.memory[eye]) vk_->vkFreeMemory(vk_->device, import.memory[eye], nullptr);
    }
    import = {};
}

const LinuxVulkan::Import* LinuxVulkan::import_session(uint64_t session)
{
    for (const auto& import : imports_) if (import.session == session) return &import;
    using namespace protocol;
    // The layer lives in the emulator process. Ask it for this session's
    // memory; one reconnect covers an emulator restart.
    LinuxGpuShareReply reply{};
    int fds[2]{-1, -1};
    int received = 0;
    for (int attempt = 0; attempt < 2 && reply.status != 1; ++attempt) {
        if (shareSocket_ < 0) {
            const auto path = linux_gpu_share_socket_path();
            sockaddr_un address{}; address.sun_family = AF_UNIX;
            if (path.size() >= sizeof(address.sun_path)) return nullptr;
            std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
            shareSocket_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
            if (shareSocket_ < 0) return nullptr;
            if (connect(shareSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
                std::fprintf(stderr, "AXRB GPU: emulator layer socket %s unavailable; is AXRB_GPU_SHARING=1 set for the emulator?\n", path.c_str());
                close(shareSocket_); shareSocket_ = -1; return nullptr;
            }
        }
        LinuxGpuShareRequest request{}; request.session = session;
        char control[CMSG_SPACE(sizeof(fds))]{};
        iovec iov{&reply, sizeof(reply)};
        msghdr message{}; message.msg_iov = &iov; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof(control);
        if (send(shareSocket_, &request, sizeof(request), MSG_NOSIGNAL) != static_cast<ssize_t>(sizeof(request)) ||
            recvmsg(shareSocket_, &message, MSG_CMSG_CLOEXEC) != static_cast<ssize_t>(sizeof(reply))) {
            close(shareSocket_); shareSocket_ = -1; reply = {}; continue;
        }
        for (cmsghdr* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
            received = static_cast<int>((header->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            std::memcpy(fds, CMSG_DATA(header), sizeof(int) * std::min(received, 2));
        }
        break;
    }
    auto closeAll = [&] { for (int fd : fds) if (fd >= 0) close(fd); };
    if (reply.magic != kLinuxGpuShareMagic || reply.status != 1 || reply.session != session ||
        reply.eye_count < 1 || reply.eye_count > 2 || received != static_cast<int>(reply.eye_count)) {
        std::fprintf(stderr, "AXRB GPU: emulator layer has no export session %016llx\n", static_cast<unsigned long long>(session));
        closeAll(); return nullptr;
    }
    if (std::memcmp(reply.device_uuid, vk_->deviceUUID, VK_UUID_SIZE) || std::memcmp(reply.driver_uuid, vk_->driverUUID, VK_UUID_SIZE)) {
        std::fprintf(stderr, "AXRB GPU: the emulator renders on a different GPU or driver than the OpenXR runtime; OPAQUE_FD sharing is impossible\n");
        closeAll(); return nullptr;
    }
    Import import{};
    import.session = session; import.eyes = reply.eye_count;
    import.width = reply.eyes[0].width; import.height = reply.eyes[0].height;
    import.format = static_cast<VkFormat>(reply.eyes[0].format);
    auto& vk = *vk_;
    for (uint32_t eye = 0; eye < reply.eye_count; ++eye) {
        const auto& source = reply.eyes[eye];
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.pNext = &external;
        info.imageType = VK_IMAGE_TYPE_2D; info.format = static_cast<VkFormat>(source.format);
        info.extent = {source.width, source.height, 1}; info.mipLevels = 1; info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = source.usage;
        bool imported = ok(vk.vkCreateImage(vk.device, &info, nullptr, &import.images[eye]), "import vkCreateImage");
        if (imported) {
            VkImportMemoryFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
            fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT; fdInfo.fd = fds[eye];
            VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
            dedicated.pNext = &fdInfo; dedicated.image = import.images[eye];
            VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocation.pNext = source.dedicated ? static_cast<const void*>(&dedicated) : &fdInfo;
            allocation.allocationSize = source.allocation_size; allocation.memoryTypeIndex = source.memory_type;
            imported = ok(vk.vkAllocateMemory(vk.device, &allocation, nullptr, &import.memory[eye]), "import vkAllocateMemory");
            if (imported) fds[eye] = -1; // A successful import owns the descriptor.
            imported = imported && ok(vk.vkBindImageMemory(vk.device, import.images[eye], import.memory[eye], 0), "import vkBindImageMemory");
        }
        if (!imported || source.width != import.width || source.height != import.height || source.format != reply.eyes[0].format) {
            destroy(import); closeAll(); return nullptr;
        }
    }
    std::fprintf(stderr, "AXRB GPU: imported export session %016llx: %u eye image(s) %ux%u format=%d (OPAQUE_FD, no pixel transfer)\n",
        static_cast<unsigned long long>(session), import.eyes, import.width, import.height, import.format);
    // Reception is synchronous, so no older import is in use here.
    while (imports_.size() >= 16) { destroy(imports_.front()); imports_.pop_front(); }
    imports_.push_back(import);
    return &imports_.back();
}

std::shared_ptr<HostImage> LinuxVulkan::acquire_image(uint32_t width, uint32_t height, VkFormat format)
{
    std::unique_ptr<HostImage> image;
    {
        std::lock_guard lock(pool_->mutex);
        auto& free = pool_->free;
        auto found = std::find_if(free.begin(), free.end(), [&](const auto& candidate) {
            return candidate->width == width && candidate->height == height && candidate->format == format;
        });
        if (found != free.end()) { image = std::move(*found); free.erase(found); }
    }
    if (!image) {
        auto& vk = *vk_;
        image = std::make_unique<HostImage>();
        image->vk = vk_;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D; info.format = format; info.extent = {width, height, 1};
        info.mipLevels = 1; info.arrayLayers = 2; info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL; info.usage = kHostImageUsage;
        if (!ok(vk.vkCreateImage(vk.device, &info, nullptr, &image->image), "vkCreateImage")) return {};
        VkMemoryRequirements requirements{}; vk.vkGetImageMemoryRequirements(vk.device, image->image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = vk.memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocation.memoryTypeIndex == UINT32_MAX ||
            !ok(vk.vkAllocateMemory(vk.device, &allocation, nullptr, &image->memory), "vkAllocateMemory") ||
            !ok(vk.vkBindImageMemory(vk.device, image->image, image->memory, 0), "vkBindImageMemory")) return {};
        image->width = width; image->height = height; image->format = format;
    }
    // Readers pin images through shared ownership; the last one returns it.
    return {image.release(), [pool = std::weak_ptr<ImagePool>(pool_)](HostImage* released) {
        if (auto owner = pool.lock()) {
            std::lock_guard lock(owner->mutex);
            if (owner->free.size() < 16) { owner->free.emplace_back(released); return; }
        }
        delete released;
    }};
}

std::shared_ptr<GpuFrameBatch> LinuxVulkan::receive(const std::vector<protocol::GpuBatchPart>& parts)
{
    static protocol::PerfStats stats("host-gpu-receive");
    protocol::PerfScope scope(stats);
    auto batch = std::make_shared<GpuFrameBatch>();
    std::vector<const Import*> imports;
    for (const auto& part : parts) {
        const auto& gpu = part.gpu;
        if (!gpu.session || (gpu.formats[1] && gpu.formats[0] != gpu.formats[1])) return {};
        const Import* import = import_session(gpu.session);
        if (!import || import->width != part.header.width || import->height != part.header.height ||
            static_cast<uint32_t>(import->format) != gpu.formats[0] || import->eyes != (gpu.formats[1] ? 2u : 1u)) return {};
        auto image = acquire_image(import->width, import->height, import->format);
        if (!image) return {};
        image->eyes = import->eyes;
        imports.push_back(import);
        batch->parts.push_back({std::move(image), part.header, part.projection});
    }
    // imports_ may have been reordered by eviction while importing later parts.
    for (size_t i = 0; i < parts.size(); ++i) {
        imports[i] = nullptr;
        for (const auto& import : imports_) if (import.session == parts[i].gpu.session) imports[i] = &import;
        if (!imports[i]) return {};
    }
    auto& vk = *vk_;
    if (!begin(receive_)) return {};
    auto cmd = receive_.cmd;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto* import = imports[i];
        auto& image = *batch->parts[i].image;
        // Same GPU and driver: acquire the emulator's images from the external
        // queue family. Its fence completed before the guest sent this frame.
        for (uint32_t eye = 0; eye < import->eyes; ++eye)
            barrier(cmd, import->images[eye], 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                0, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_EXTERNAL, vk.family);
        barrier(cmd, image.image, 2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        for (uint32_t eye = 0; eye < import->eyes; ++eye) {
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
            region.extent = {image.width, image.height, 1};
            vk.vkCmdCopyImage(cmd, import->images[eye], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        for (uint32_t eye = 0; eye < import->eyes; ++eye)
            barrier(cmd, import->images[eye], 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_TRANSFER_READ_BIT, 0, vk.family, VK_QUEUE_FAMILY_EXTERNAL);
        barrier(cmd, image.image, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    }
    if (!submit_and_wait(receive_)) return {};
    return batch;
}

bool LinuxVulkan::ensure_staging(VkDeviceSize bytes)
{
    if (stagingMapped_ && stagingBytes_ >= bytes) return true;
    auto& vk = *vk_;
    if (staging_) vk.vkDestroyBuffer(vk.device, staging_, nullptr);
    if (stagingMemory_) vk.vkFreeMemory(vk.device, stagingMemory_, nullptr);
    staging_ = VK_NULL_HANDLE; stagingMemory_ = VK_NULL_HANDLE; stagingMapped_ = nullptr; stagingBytes_ = 0;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes; info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!ok(vk.vkCreateBuffer(vk.device, &info, nullptr, &staging_), "vkCreateBuffer")) return false;
    VkMemoryRequirements requirements{}; vk.vkGetBufferMemoryRequirements(vk.device, staging_, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = vk.memory_type(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX ||
        !ok(vk.vkAllocateMemory(vk.device, &allocation, nullptr, &stagingMemory_), "vkAllocateMemory") ||
        !ok(vk.vkBindBufferMemory(vk.device, staging_, stagingMemory_, 0), "vkBindBufferMemory") ||
        !ok(vk.vkMapMemory(vk.device, stagingMemory_, 0, bytes, 0, &stagingMapped_), "vkMapMemory")) return false;
    stagingBytes_ = bytes;
    return true;
}

bool LinuxVulkan::fill(VkImage target, VkFormat targetFormat, uint32_t width, uint32_t height,
                       const HostImage* gpu, const std::vector<uint8_t>* pixels, uint32_t pixelLayers,
                       const char* capturePath)
{
    auto& vk = *vk_;
    const bool bgra = targetFormat == VK_FORMAT_B8G8R8A8_SRGB || targetFormat == VK_FORMAT_B8G8R8A8_UNORM;
    if (!gpu) {
        // Pixel transport: rows arrive bottom-up (GLES convention). Flip while
        // staging; the bytes go into the swapchain unconverted, as on Windows.
        const size_t layerBytes = static_cast<size_t>(width) * height * 4;
        if (!pixels || !pixelLayers || pixels->size() < layerBytes * pixelLayers || !ensure_staging(layerBytes * 2)) return false;
        for (uint32_t eye = 0; eye < 2; ++eye) {
            const uint8_t* source = pixels->data() + layerBytes * (pixelLayers > 1 ? eye % pixelLayers : 0);
            auto* destination = static_cast<uint8_t*>(stagingMapped_) + layerBytes * eye;
            for (uint32_t y = 0; y < height; ++y) {
                const uint8_t* row = source + static_cast<size_t>(height - 1 - y) * width * 4;
                uint8_t* out = destination + static_cast<size_t>(y) * width * 4;
                if (!bgra) { std::memcpy(out, row, static_cast<size_t>(width) * 4); continue; }
                for (uint32_t x = 0; x < width * 4; x += 4) {
                    out[x] = row[x + 2]; out[x + 1] = row[x + 1]; out[x + 2] = row[x]; out[x + 3] = row[x + 3];
                }
            }
        }
    } else if (gpu->width < width || gpu->height < height) return false;
    if (!begin(present_)) return false;
    auto cmd = present_.cmd;
    barrier(cmd, target, 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        if (gpu) {
            const uint32_t sourceLayer = gpu->eyes == 1 ? 0 : eye;
            if (gpu->format == targetFormat) {
                VkImageCopy region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceLayer, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
                region.extent = {width, height, 1};
                vk.vkCmdCopyImage(cmd, gpu->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            } else {
                // Format conversion (sRGB encoding, channel order) without shaders.
                VkImageBlit region{};
                region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sourceLayer, 1};
                region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
                region.srcOffsets[1] = region.dstOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
                vk.vkCmdBlitImage(cmd, gpu->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
            }
        } else {
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(width) * height * 4 * eye;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1};
            region.imageExtent = {width, height, 1};
            vk.vkCmdCopyBufferToImage(cmd, staging_, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
    }
    const VkDeviceSize captureBytes = static_cast<VkDeviceSize>(width) * height * 4;
    if (capturePath && captureBytes_ < captureBytes) {
        if (capture_) vk.vkDestroyBuffer(vk.device, capture_, nullptr);
        if (captureMemory_) vk.vkFreeMemory(vk.device, captureMemory_, nullptr);
        capture_ = VK_NULL_HANDLE; captureMemory_ = VK_NULL_HANDLE; captureMapped_ = nullptr; captureBytes_ = 0;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = captureBytes; info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VkMemoryRequirements requirements{};
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        if (ok(vk.vkCreateBuffer(vk.device, &info, nullptr, &capture_), "capture buffer")) {
            vk.vkGetBufferMemoryRequirements(vk.device, capture_, &requirements);
            allocation.allocationSize = requirements.size;
            allocation.memoryTypeIndex = vk.memory_type(requirements.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            if (allocation.memoryTypeIndex != UINT32_MAX &&
                ok(vk.vkAllocateMemory(vk.device, &allocation, nullptr, &captureMemory_), "capture memory") &&
                ok(vk.vkBindBufferMemory(vk.device, capture_, captureMemory_, 0), "capture bind") &&
                ok(vk.vkMapMemory(vk.device, captureMemory_, 0, captureBytes, 0, &captureMapped_), "capture map"))
                captureBytes_ = captureBytes;
        }
    }
    const bool capture = capturePath && captureMapped_ && captureBytes_ >= captureBytes;
    if (capture) {
        barrier(cmd, target, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vk.vkCmdCopyImageToBuffer(cmd, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture_, 1, &region);
        barrier(cmd, target, 2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    } else {
        barrier(cmd, target, 2, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    }
    if (capture) {
        if (!submit_and_wait(present_)) return false;
        // Left eye as presented: 8-bit values in the swapchain's own encoding.
        if (FILE* file = std::fopen(capturePath, "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", width, height);
            const auto* bytes = static_cast<const uint8_t*>(captureMapped_);
            std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const uint8_t* pixel = bytes + (static_cast<size_t>(y) * width + x) * 4;
                    row[x * 3] = pixel[bgra ? 2 : 0]; row[x * 3 + 1] = pixel[1]; row[x * 3 + 2] = pixel[bgra ? 0 : 2];
                }
                std::fwrite(row.data(), 1, row.size(), file);
            }
            std::fclose(file);
            std::fprintf(stderr, "AXRB diagnostic capture: %s\n", capturePath);
        }
        return true;
    }
    // Waiting keeps the pinned source frame and staging buffer simple to own;
    // two-layer copies take well under a millisecond on the GPU.
    return submit_and_wait(present_);
}

} // namespace axrb::host
#endif
