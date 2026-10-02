#pragma once
// Allocations of an uncached HOST_COHERENT system-memory type are made from a
// cached coherent type of the same heap instead.
//
// On a Linux/KVM host with an NVIDIA GPU, gfxstream's memory type 3
// (HOST_VISIBLE|HOST_COHERENT, not cached, not device-local) is not coherent
// in the guest: the guest CPU writes it at cached speed (about 2 GB/s, where
// Windows/WHPX maps it uncached), and the GPU does not see those writes, nor
// the guest its GPU writes, at cache-line granularity. Staging data for
// textures arrives partly stale, which is the striped, block-patched texture
// corruption of UE titles on Linux (docs/linux_spike.md;
// tests/android/vulkan_etc2_android.cpp reproduces it without a game). The
// cached type of the same heap (HOST_CACHED added) and the device-local
// host-visible type are coherent.
//
// The application still sees the index it asked for; the substituted type has
// every property flag of the original. Buffer and image requirements drop a
// source type when the resource could not be bound to its substitute, so
// memory of that index is only ever bound to resources that accept both.
// Off unless debug.axrb.coherent_memory=1, which the Linux emulator script
// sets. Windows/WHPX maps type 3 uncached (docs/cpu_pressure.md); whether it
// needs the remap there has not been tested.
#include <vulkan/vulkan.h>
#include <sys/system_properties.h>
#include <cstring>
namespace axrb {
inline bool coherent_memory_enabled() {
    static const bool enabled = [] {
        char value[PROP_VALUE_MAX]{};
        __system_property_get("debug.axrb.coherent_memory", value);
        return std::strcmp(value, "1") == 0;
    }();
    return enabled;
}
struct CoherentMemoryPolicy {
    uint32_t sources = 0;           // bit i: allocations of type i are remapped
    uint32_t target[VK_MAX_MEMORY_TYPES]{};
    static CoherentMemoryPolicy from(const VkPhysicalDeviceMemoryProperties& p) {
        CoherentMemoryPolicy result;
        constexpr VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t i = 0; i < p.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = p.memoryTypes[i].propertyFlags;
            if ((flags & coherent) != coherent ||
                (flags & (VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
                continue;
            const VkMemoryPropertyFlags wanted = flags | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            for (uint32_t j = 0; j < p.memoryTypeCount; ++j)
                if (j != i && p.memoryTypes[j].heapIndex == p.memoryTypes[i].heapIndex &&
                    (p.memoryTypes[j].propertyFlags & wanted) == wanted &&
                    !(p.memoryTypes[j].propertyFlags & ~wanted & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    result.sources |= 1u << i;
                    result.target[i] = j;
                    break;
                }
        }
        return result;
    }
    uint32_t allocation_type(uint32_t type) const { return type < VK_MAX_MEMORY_TYPES && (sources >> type & 1) ? target[type] : type; }
    // A resource that cannot live in a source type's substitute must not be
    // given that source type; it keeps it only if it has no other choice.
    uint32_t filter(uint32_t bits) const {
        uint32_t out = bits;
        for (uint32_t i = 0; i < VK_MAX_MEMORY_TYPES; ++i)
            if ((sources >> i & 1) && (bits >> i & 1) && !(bits >> target[i] & 1) && (out & ~(1u << i))) out &= ~(1u << i);
        return out;
    }
};
} // namespace axrb
