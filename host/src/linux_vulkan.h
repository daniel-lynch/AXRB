#pragma once
// Linux host presentation: the Vulkan device the OpenXR runtime is bound to
// (XR_KHR_vulkan_enable2), reception of the emulator's shared eye images
// (OPAQUE_FD, see protocol/linux_gpu_share.h) into host-owned copies, and the
// per-frame copy into the runtime's swapchain. Transfer commands only: no
// shaders, so no shader toolchain is needed to build the host on Linux.
#if !defined(_WIN32)
#include "gpu_frame_packet.h"
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace axrb::host {

#define AXRB_VK_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetDeviceProcAddr)
#define AXRB_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkCreateFence) X(vkDestroyFence) X(vkResetFences) X(vkWaitForFences) X(vkQueueSubmit) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdBlitImage) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer)

// Owns the instance and device; frames that outlive the presenter keep it alive.
struct VulkanContext {
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define AXRB_DECLARE(name) PFN_##name name = nullptr;
    AXRB_VK_INSTANCE_FUNCTIONS(AXRB_DECLARE)
    AXRB_VK_DEVICE_FUNCTIONS(AXRB_DECLARE)
#undef AXRB_DECLARE
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    uint8_t deviceUUID[VK_UUID_SIZE]{}, driverUUID[VK_UUID_SIZE]{};
    uint32_t family = 0;
    ~VulkanContext();
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags flags) const;
};

// One received layer in host-owned GPU memory: both eyes (or both packed
// quads) as array layers. Returned to the presenter's pool when released.
struct HostImage {
    std::shared_ptr<VulkanContext> vk;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t width = 0, height = 0, eyes = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    ~HostImage();
};

// A complete received frame: every composition layer in application order.
struct GpuFrameBatch {
    struct Part {
        std::shared_ptr<HostImage> image;
        protocol::ImageFrameHeader header{};
        protocol::ImageProjection projection{};
    };
    std::vector<Part> parts;
};

class LinuxVulkan {
public:
    ~LinuxVulkan();
    // Creates the instance and device through the runtime (XR_KHR_vulkan_enable2).
    bool create(XrInstance instance, XrSystemId system, PFN_xrGetInstanceProcAddr getProcAddr);
    const XrGraphicsBindingVulkan2KHR* binding() const { return &binding_; }
    // Receive thread: copy the emulator's shared images of every layer into
    // pooled host images with one submission. Returns null when the frame
    // must be refused (NACK; the guest then stops reusing its shared images).
    std::shared_ptr<GpuFrameBatch> receive(const std::vector<protocol::GpuBatchPart>& parts);
    // Frame thread: write one layer into an acquired swapchain image (two
    // array layers, COLOR_ATTACHMENT_OPTIMAL before and after), either from a
    // received GPU image or from transported pixels (bottom-up rows).
    bool fill(VkImage target, VkFormat targetFormat, uint32_t width, uint32_t height,
              const HostImage* gpu, const std::vector<uint8_t>* pixels = nullptr, uint32_t pixelLayers = 0,
              const char* capturePath = nullptr);

private:
    struct Commands {
        VkQueue queue = VK_NULL_HANDLE;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };
    struct Import {
        uint64_t session = 0;
        uint32_t eyes = 0, width = 0, height = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        std::array<VkImage, 2> images{};
        std::array<VkDeviceMemory, 2> memory{};
    };
    bool create_commands(Commands& commands, uint32_t queueIndex);
    bool begin(Commands& commands);
    bool submit_and_wait(Commands& commands);
    void barrier(VkCommandBuffer cmd, VkImage image, uint32_t layers, VkImageLayout before, VkImageLayout after,
                 VkAccessFlags src, VkAccessFlags dst, uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
                 uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED);
    const Import* import_session(uint64_t session);
    void destroy(Import& import);
    bool ensure_staging(VkDeviceSize bytes);
    std::shared_ptr<HostImage> acquire_image(uint32_t width, uint32_t height, VkFormat format);
    struct ImagePool {
        std::mutex mutex;
        std::vector<std::unique_ptr<HostImage>> free;
    };

    std::shared_ptr<VulkanContext> vk_;
    XrGraphicsBindingVulkan2KHR binding_{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    Commands present_, receive_;
    std::mutex sharedQueueMutex_; // only when the family has a single queue
    bool sharedQueue_ = false;
    std::deque<Import> imports_;
    int shareSocket_ = -1;
    std::shared_ptr<ImagePool> pool_ = std::make_shared<ImagePool>();
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory_ = VK_NULL_HANDLE;
    void* stagingMapped_ = nullptr;
    VkDeviceSize stagingBytes_ = 0;
    // Opt-in diagnostic readback (AXRB_CAPTURE_PREFIX); never on the normal path.
    VkBuffer capture_ = VK_NULL_HANDLE;
    VkDeviceMemory captureMemory_ = VK_NULL_HANDLE;
    void* captureMapped_ = nullptr;
    VkDeviceSize captureBytes_ = 0;
};

} // namespace axrb::host
#endif
