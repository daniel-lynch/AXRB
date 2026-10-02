#pragma once
// Linux counterpart of the Windows named D3D11 shared textures. The emulator's
// Vulkan layer (host/gpu/layer.cpp) exports each eye image as Vulkan OPAQUE_FD
// memory and serves the file descriptors on a Unix socket; the host bridge
// pulls them by export session (the id carried in WindowsGpuFrame) and imports
// them into its own Vulkan device on the same physical GPU.
#include <cstdint>
#include <cstdlib>
#include <string>

namespace axrb::protocol {
constexpr uint32_t kLinuxGpuShareMagic = 0x53475841; // "AXGS"
constexpr uint32_t kLinuxGpuShareVersion = 1;

struct LinuxGpuShareRequest {
    uint32_t magic = kLinuxGpuShareMagic;
    uint32_t version = kLinuxGpuShareVersion;
    uint64_t session = 0;
};
static_assert(sizeof(LinuxGpuShareRequest) == 16);

// Everything the importer must repeat exactly: OPAQUE_FD imports require the
// same image parameters, allocation size and memory type as the export.
struct LinuxGpuShareImage {
    uint32_t width = 0, height = 0;
    uint32_t format = 0;          // VkFormat
    uint32_t usage = 0;           // VkImageUsageFlags of the exported image
    uint64_t allocation_size = 0;
    uint32_t memory_type = 0;
    uint32_t dedicated = 1;
};
static_assert(sizeof(LinuxGpuShareImage) == 32);

// One SCM_RIGHTS descriptor per eye accompanies a reply with status 1.
struct LinuxGpuShareReply {
    uint32_t magic = kLinuxGpuShareMagic;
    uint32_t version = kLinuxGpuShareVersion;
    uint64_t session = 0;
    uint32_t status = 0;          // 1 = found; fds attached
    uint32_t eye_count = 0;
    uint8_t device_uuid[16]{};
    uint8_t driver_uuid[16]{};
    LinuxGpuShareImage eyes[2]{};
};
static_assert(sizeof(LinuxGpuShareReply) == 120);

inline std::string linux_gpu_share_socket_path()
{
    if (const char* path = std::getenv("AXRB_GPU_SHARE_SOCKET"); path && *path) return path;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    return std::string(runtime && *runtime ? runtime : "/tmp") + "/axrb-gpu-share.sock";
}
}
