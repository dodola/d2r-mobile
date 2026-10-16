/*
 * Stable identity for the Vulkan driver's pipeline cache.
 *
 * VkPipelineCache is only portable within the device/driver/profile that
 * produced it.  The Vulkan implementation also stores its own UUID in the
 * blob, but that does not cover our shader compatibility policy: the SPIR-V
 * stream changes when a quirk pass is enabled. Keep this small fixed-size
 * record next to a persisted blob and reject a stale cache before handing
 * bytes to vkCreatePipelineCache.
 */
#ifndef DM_NVN_VK_PIPELINE_CACHE_KEY_H
#define DM_NVN_VK_PIPELINE_CACHE_KEY_H

#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "nvn_vk_profile.h"

#define NVN_VK_PIPELINE_CACHE_KEY_VERSION 2u
/* Bump when shader rewriting or pipeline interpretation changes incompatibly. */
#define NVN_VK_PIPELINE_SHADER_ABI 2u

typedef struct {
    uint32_t version;
    uint32_t api_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_id;
    uint32_t driver_version;
    uint32_t quirk_flags;
    uint32_t render_tier;
    uint32_t subgroup_size;
    uint32_t subgroup_stages;
    uint32_t subgroup_operations;
    uint32_t enabled_features;
    uint32_t shader_abi;
    uint8_t pipeline_cache_uuid[VK_UUID_SIZE];
} NvnVkPipelineCacheKey;

/* A disk-cache writer serializes this record beside the Vulkan blob. */
_Static_assert(sizeof(NvnVkPipelineCacheKey) == 68,
    "Vulkan pipeline cache identity layout changed");

static inline NvnVkPipelineCacheKey nvn_vk_pipeline_cache_key_make(
    const VkPhysicalDeviceProperties *properties,
    const NvnVkGpuProfile *profile,
    const NvnVkQuirkRegistry *registry) {
    NvnVkPipelineCacheKey key;
    memset(&key, 0, sizeof key);
    key.version = NVN_VK_PIPELINE_CACHE_KEY_VERSION;
    key.shader_abi = NVN_VK_PIPELINE_SHADER_ABI;
    if (properties) {
        key.api_version = properties->apiVersion;
        key.vendor_id = properties->vendorID;
        key.device_id = properties->deviceID;
        key.driver_version = properties->driverVersion;
        memcpy(key.pipeline_cache_uuid, properties->pipelineCacheUUID,
            sizeof key.pipeline_cache_uuid);
    }
    if (profile) {
        key.api_version = profile->api_version;
        key.vendor_id = profile->vendor_id;
        key.device_id = profile->device_id;
        key.driver_id = profile->driver_id;
        key.driver_version = profile->driver_version;
        key.subgroup_size = profile->subgroup_size;
        key.subgroup_stages = profile->subgroup_stages;
        key.subgroup_operations = profile->subgroup_operations;
    }
    if (registry) {
        key.quirk_flags = registry->flags;
        key.render_tier = (uint32_t)registry->render_tier;
    }
    return key;
}

static inline int nvn_vk_pipeline_cache_key_equal(
    const NvnVkPipelineCacheKey *a, const NvnVkPipelineCacheKey *b) {
    return a && b && memcmp(a, b, sizeof *a) == 0;
}

#endif
