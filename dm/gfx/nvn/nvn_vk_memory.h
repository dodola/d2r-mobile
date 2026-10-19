#ifndef NVN_VK_MEMORY_H
#define NVN_VK_MEMORY_H
#include <stdint.h>

/* Backend-owned VkDeviceMemory allocations, not physical residency or driver
 * mtrack. Sizes are the actual VkMemoryRequirements allocation sizes. */
enum {
    DM_VK_MEMORY_TEXTURE, DM_VK_MEMORY_SAMPLE_COPY, DM_VK_MEMORY_SHADOW,
    DM_VK_MEMORY_POOL, DM_VK_MEMORY_SUPPORT, DM_VK_MEMORY_TEMP,
    DM_VK_MEMORY_WRITEBACK, DM_VK_MEMORY_READBACK, DM_VK_MEMORY_PRESENT,
    DM_VK_MEMORY_CLASS_COUNT
};
typedef struct {
    uint32_t abi_version,class_count;
    uint64_t bytes[DM_VK_MEMORY_CLASS_COUNT],peak_bytes[DM_VK_MEMORY_CLASS_COUNT];
    uint64_t allocations[DM_VK_MEMORY_CLASS_COUNT];
    uint64_t total_bytes,peak_total_bytes;
    uint64_t cpu_baseline_bytes,cpu_baseline_peak_bytes;
    uint64_t temp_recording_bytes,temp_inflight_bytes,temp_retired_bytes;
    uint64_t trim_count,last_trim_freed_bytes,last_trim_ns;
} DmVkMemoryStats;
enum {
    DM_VK_TRIM_RETIRED=1u, DM_VK_TRIM_DERIVED=2u, DM_VK_TRIM_WRITEBACK=4u,
    DM_VK_TRIM_ALL=7u
};

/* Both functions use trylock and return EBUSY without waiting on a recorder or
 * GPU. They leave *out unchanged on error and never initialize Vulkan. */
int dm_vk_memory_get_stats(DmVkMemoryStats *out);
/* Caller must serialize lifecycle transitions and suspend/drain guest graphics
 * first. Only retired buffers and reconstructable derived images are released;
 * primary textures, guest pools and their CPU baselines remain intact. */
int dm_vk_memory_trim_caches(unsigned flags,DmVkMemoryStats *out);
#endif
