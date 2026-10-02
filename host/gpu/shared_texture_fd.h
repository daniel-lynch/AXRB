#pragma once
// Linux shared eye images for the emulator-side layer: Vulkan memory exported
// as OPAQUE_FD. The host bridge imports the same payload on the same GPU
// (matching device and driver UUIDs), so no pixel ever leaves video memory.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace axrb {
struct SharedTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    int fd = -1;
    VkDeviceSize size = 0;
    uint32_t memoryType = 0;
    uint32_t width = 0, height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
};

struct SharedDevice {
    VkDevice device{};
    PFN_vkGetDeviceProcAddr gdpa{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceMemoryProperties memory{};
    uint8_t deviceUUID[VK_UUID_SIZE]{};
    uint8_t driverUUID[VK_UUID_SIZE]{};
    PFN_vkGetMemoryFdKHR getMemoryFd{};
    bool initialize(VkInstance instance, VkPhysicalDevice gpu, VkDevice vkDevice,
                    PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr getDeviceProc) {
        physical = gpu; device = vkDevice; gdpa = getDeviceProc;
        auto memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
        auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2"));
        if (!getProps) getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(gipa(instance, "vkGetPhysicalDeviceProperties2KHR"));
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(gdpa(device, "vkGetMemoryFdKHR"));
        if (!memoryProperties || !getProps || !getMemoryFd) return false;
        memoryProperties(gpu, &memory);
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; props.pNext = &id;
        getProps(gpu, &props);
        std::memcpy(deviceUUID, id.deviceUUID, VK_UUID_SIZE);
        std::memcpy(driverUUID, id.driverUUID, VK_UUID_SIZE);
        std::fprintf(stderr, "AXRB GPU: %s exports eye images as Vulkan OPAQUE_FD memory\n", props.properties.deviceName);
        return true;
    }
    bool create(SharedTexture& out, uint32_t width, uint32_t height, VkFormat format) {
        auto createImage = reinterpret_cast<PFN_vkCreateImage>(gdpa(device, "vkCreateImage"));
        auto getRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(gdpa(device, "vkGetImageMemoryRequirements"));
        auto allocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(gdpa(device, "vkAllocateMemory"));
        auto bindMemory = reinterpret_cast<PFN_vkBindImageMemory>(gdpa(device, "vkBindImageMemory"));
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; imageInfo.pNext = &external;
        imageInfo.imageType = VK_IMAGE_TYPE_2D; imageInfo.format = format;
        imageInfo.extent = {width, height, 1}; imageInfo.mipLevels = 1; imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VkResult result = createImage(device, &imageInfo, nullptr, &out.image);
        if (result != VK_SUCCESS) { std::fprintf(stderr, "AXRB GPU: CreateImage=%d\n", result); return false; }
        VkMemoryRequirements req{}; getRequirements(device, out.image, &req);
        uint32_t index = 0;
        while (index < memory.memoryTypeCount && (!(req.memoryTypeBits & (1u << index)) ||
               !(memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))) ++index;
        if (index == memory.memoryTypeCount) { destroy(out); return false; }
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.pNext = &exportInfo; dedicated.image = out.image;
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.pNext = &dedicated; allocation.allocationSize = req.size; allocation.memoryTypeIndex = index;
        result = allocateMemory(device, &allocation, nullptr, &out.memory);
        if (result != VK_SUCCESS) { std::fprintf(stderr, "AXRB GPU: export memory=%d\n", result); destroy(out); return false; }
        result = bindMemory(device, out.image, out.memory, 0);
        VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        fdInfo.memory = out.memory; fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (result != VK_SUCCESS || getMemoryFd(device, &fdInfo, &out.fd) != VK_SUCCESS || out.fd < 0) {
            std::fprintf(stderr, "AXRB GPU: bind/export fd failed (%d)\n", result); destroy(out); return false;
        }
        out.size = req.size; out.memoryType = index; out.width = width; out.height = height;
        out.format = format; out.usage = imageInfo.usage;
        std::fprintf(stderr, "AXRB GPU: shared image %ux%u format=%d bytes=%llu type=%u fd=%d\n", width, height, format,
            static_cast<unsigned long long>(req.size), index, out.fd);
        return true;
    }
    void destroy(SharedTexture& texture) {
        if (texture.image) reinterpret_cast<PFN_vkDestroyImage>(gdpa(device, "vkDestroyImage"))(device, texture.image, nullptr);
        if (texture.memory) reinterpret_cast<PFN_vkFreeMemory>(gdpa(device, "vkFreeMemory"))(device, texture.memory, nullptr);
        if (texture.fd >= 0) close(texture.fd);
        texture = {};
    }
};
}
