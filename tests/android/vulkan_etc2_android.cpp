// Isolates the emulator's ETC2 emulation from any game: uploads ETC2/EAC
// blocks to a mipmapped image the way an engine does (one buffer-to-image copy
// per mip level, levels packed back to back in one staging buffer), reads
// every texel of every level back through a compute shader (texelFetch) and
// writes them to a file. tools/compare_etc2.py decodes the same blocks on the
// host and compares.
//   vulkan_etc2_android <blocks.bin> <vkformat> <width> <height> <mips> <out.rgba> [images] [dedicated]
// <blocks.bin> holds the levels' blocks in order, for each image in turn.
// With several images they are bound one after the other in one memory
// allocation at the offsets their requirements give, as engines sub-allocate
// (UE4's resource heaps); "dedicated" gives each image its own allocation. Run as a plain shell binary,
// so no Vulkan layer is loaded: gfxstream alone decodes.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "astc_fetch_spirv.h" // fetch.comp: texelFetch of every texel of one level, packUnorm4x8 into a buffer

#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) { std::fprintf(stderr, "%s: %d at line %d\n", #call, r_, __LINE__); return 1; } } while (0)

static uint32_t block_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK: case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
    case VK_FORMAT_EAC_R11_UNORM_BLOCK: case VK_FORMAT_EAC_R11_SNORM_BLOCK: return 8;
    default: return 16;
    }
}

int main(int argc, char** argv) {
    if (argc < 7) { std::fprintf(stderr, "usage: %s <blocks.bin> <vkformat> <width> <height> <mips> <out.rgba>\n", argv[0]); return 2; }
    setbuf(stdout, nullptr);
    const VkFormat format = VkFormat(std::atoi(argv[2]));
    const uint32_t width = uint32_t(std::atoi(argv[3])), height = uint32_t(std::atoi(argv[4])), mips = uint32_t(std::atoi(argv[5]));
    const uint32_t count = argc > 7 ? uint32_t(std::atoi(argv[7])) : 1;
    const bool dedicated = argc > 8 && std::strstr(argv[8], "dedicated");
    const bool serial = argc > 8 && std::strstr(argv[8], "serial"); // one submit (and wait) per image
    const bool cached = argc > 8 && std::strstr(argv[8], "cached");  // staging and readback in HOST_CACHED memory
    const bool bar = argc > 8 && std::strstr(argv[8], "bar");        // ... or in DEVICE_LOCAL host-visible memory
    std::vector<uint8_t> blocks;
    if (FILE* f = std::fopen(argv[1], "rb")) {
        std::fseek(f, 0, SEEK_END); blocks.resize(size_t(std::ftell(f))); std::fseek(f, 0, SEEK_SET);
        if (std::fread(blocks.data(), 1, blocks.size(), f) != blocks.size()) blocks.clear();
        std::fclose(f);
    }
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize bytes = 0, texels = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t w = std::max(1u, width >> m), h = std::max(1u, height >> m);
        VkBufferImageCopy c{};
        c.bufferOffset = bytes;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        c.imageExtent = {w, h, 1};
        copies.push_back(c);
        bytes += VkDeviceSize((w + 3) / 4) * ((h + 3) / 4) * block_bytes(format);
        texels += VkDeviceSize(w) * h;
    }
    if (blocks.size() < bytes * count) { std::fprintf(stderr, "need %llu bytes of blocks, have %zu\n", (unsigned long long)(bytes * count), blocks.size()); return 3; }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "AXRB ETC2 isolation";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    CHECK(vkCreateInstance(&ici, nullptr, &instance));
    uint32_t n = 1;
    VkPhysicalDevice physical;
    vkEnumeratePhysicalDevices(instance, &n, &physical);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
    std::printf("Device: %s; textureCompressionETC2=%u ASTC_LDR=%u BC=%u; format %d optimal features 0x%x\n", props.deviceName,
                supported.textureCompressionETC2, supported.textureCompressionASTC_LDR, supported.textureCompressionBC, int(format),
                fp.optimalTilingFeatures);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) std::printf("memory type %u: flags 0x%x heap %u\n", i, mp.memoryTypes[i].propertyFlags, mp.memoryTypes[i].heapIndex);
    auto memoryType = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
        std::abort();
    };
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
    uint32_t family = 0;
    while (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    features.textureCompressionETC2 = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.pEnabledFeatures = &features;
    VkDevice device;
    CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue;
    vkGetDeviceQueue(device, family, 0, &queue);

    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {width, height, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    std::vector<VkImage> images(count);
    std::vector<VkDeviceSize> offsets(count);
    VkMemoryRequirements req{};
    VkDeviceSize heap = 0;
    uint32_t typeBits = ~0u;
    for (uint32_t k = 0; k < count; ++k) {
        CHECK(vkCreateImage(device, &ii, nullptr, &images[k]));
        vkGetImageMemoryRequirements(device, images[k], &req);
        heap = (heap + req.alignment - 1) / req.alignment * req.alignment;
        offsets[k] = heap;
        heap += req.size;
        typeBits &= req.memoryTypeBits;
    }
    std::printf("%u image(s): requirement %llu bytes, alignment %llu, %s\n", count, (unsigned long long)req.size,
                (unsigned long long)req.alignment, dedicated ? "one allocation each" : "packed in one allocation");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.memoryTypeIndex = memoryType(typeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    if (!dedicated) {
        mai.allocationSize = heap;
        CHECK(vkAllocateMemory(device, &mai, nullptr, &imageMemory));
    }
    for (uint32_t k = 0; k < count; ++k) {
        if (dedicated) {
            mai.allocationSize = req.size;
            CHECK(vkAllocateMemory(device, &mai, nullptr, &imageMemory));
            CHECK(vkBindImageMemory(device, images[k], imageMemory, 0));
        } else {
            CHECK(vkBindImageMemory(device, images[k], imageMemory, offsets[k]));
        }
    }

    auto makeBuffer = [&](VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buffer, VkDeviceMemory* memory, void** mapped) {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        if (vkCreateBuffer(device, &bi, nullptr, buffer)) return false;
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(device, *buffer, &r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                              (cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0) |
                                                              (bar ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0));
        std::printf("buffer of %llu bytes in memory type %u\n", (unsigned long long)size, ai.memoryTypeIndex);
        return !vkAllocateMemory(device, &ai, nullptr, memory) && !vkBindBufferMemory(device, *buffer, *memory, 0) &&
               !vkMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped);
    };
    VkBuffer staging, output;
    VkDeviceMemory stagingMemory, outputMemory;
    void *stagingMapped, *outputMapped;
    if (!makeBuffer(bytes * count, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging, &stagingMemory, &stagingMapped)) return 4;
    if (!makeBuffer(texels * 4 * count, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &output, &outputMemory, &outputMapped)) return 4;
    const auto writeStart = std::chrono::steady_clock::now();
    std::memcpy(stagingMapped, blocks.data(), bytes * count);
    std::printf("guest CPU wrote the staging buffer at %.0f MB/s\n",
                double(bytes * count) / 1e6 / std::chrono::duration<double>(std::chrono::steady_clock::now() - writeStart).count());
    std::memset(outputMapped, 0xcd, texels * 4 * count);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cai, &cmd));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    CHECK(vkCreateFence(device, &fci, nullptr, &fence));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    for (uint32_t first = 0; first < count; first += serial ? 1 : count) {
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    for (uint32_t k = first; k < (serial ? first + 1 : count); ++k) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = images[k];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    // One copy per level, as UE does.
    for (auto c : copies) {
        c.bufferOffset += bytes * k;
        vkCmdCopyBufferToImage(cmd, staging, images[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkResetFences(device, 1, &fence));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull));
    }

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.maxLod = float(mips);
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sci, nullptr, &sampler));
    VkDescriptorSetLayoutBinding bindings[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dlci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dlci.bindingCount = 2;
    dlci.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    CHECK(vkCreateDescriptorSetLayout(device, &dlci, nullptr, &setLayout));
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &setLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    VkPipelineLayout pipelineLayout;
    CHECK(vkCreatePipelineLayout(device, &plci, nullptr, &pipelineLayout));
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = sizeof(kFetchSpirv);
    smci.pCode = kFetchSpirv;
    VkShaderModule module;
    CHECK(vkCreateShaderModule(device, &smci, nullptr, &module));
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    cpci.layout = pipelineLayout;
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipeline));
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, mips * count}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, mips * count}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = mips * count;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = sizes;
    VkDescriptorPool descriptorPool;
    CHECK(vkCreateDescriptorPool(device, &dpci, nullptr, &descriptorPool));
    CHECK(vkResetCommandBuffer(cmd, 0));
    CHECK(vkBeginCommandBuffer(cmd, &begin));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    VkDeviceSize at = 0;
    for (uint32_t k = 0; k < count; ++k) {
    vci.image = images[k];
    VkImageView view;
    CHECK(vkCreateImageView(device, &vci, nullptr, &view));
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t w = std::max(1u, width >> m), h = std::max(1u, height >> m);
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = descriptorPool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        VkDescriptorSet set;
        CHECK(vkAllocateDescriptorSets(device, &dsai, &set));
        VkDescriptorImageInfo imageInfo{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bufferInfo{output, at * 4, VkDeviceSize(w) * h * 4};
        VkWriteDescriptorSet writes[2]{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo, nullptr, nullptr};
        writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufferInfo, nullptr};
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0, nullptr);
        const uint32_t constants[3] = {w, h, m};
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, constants);
        vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
        at += VkDeviceSize(w) * h;
    }
    }
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    CHECK(vkEndCommandBuffer(cmd));
    CHECK(vkResetFences(device, 1, &fence));
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, ~0ull));
    if (FILE* f = std::fopen(argv[6], "wb")) {
        std::fwrite(outputMapped, 4, texels * count, f);
        std::fclose(f);
    } else {
        return 5;
    }
    std::printf("wrote %llu texels over %u level(s) of %u image(s)\n", (unsigned long long)(texels * count), mips, count);
    vkDeviceWaitIdle(device);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
