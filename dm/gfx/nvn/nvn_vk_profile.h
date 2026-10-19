/* Device and driver compatibility policy for the Android Vulkan backend.
 *
 * The profile deliberately contains only immutable Vulkan capabilities.  The
 * quirk registry is derived from that snapshot at device creation and is the
 * single input to shader compatibility passes.  Keeping policy here makes it
 * possible to add a device rule without scattering vendor checks through the
 * replay path.
 */
#ifndef DM_NVN_VK_PROFILE_H
#define DM_NVN_VK_PROFILE_H

#include <stdint.h>
#include <vulkan/vulkan.h>

typedef enum {
    NVN_VK_RENDER_TIER_FULL = 0,
    NVN_VK_RENDER_TIER_COMPAT = 1,
    NVN_VK_RENDER_TIER_CONSERVATIVE = 2
} NvnVkRenderTier;

typedef enum {
    NVN_VK_QUIRK_NONE = 0,
    /* Qualcomm's system compiler mishandles divergent subgroup ballot. */
    NVN_VK_QUIRK_LOWER_COMPUTE_BALLOT = 1u << 0,
    /* Qualcomm's system compiler mishandles dynamic component indexing of uniform buffer arrays (vertex and fragment). */
    NVN_VK_QUIRK_LOWER_SKIN_UBO = 1u << 1
} NvnVkQuirk;

typedef struct {
    uint32_t api_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_id;
    uint32_t driver_version;
    uint32_t subgroup_size;
    VkShaderStageFlags subgroup_stages;
    VkSubgroupFeatureFlags subgroup_operations;
} NvnVkGpuProfile;

typedef struct {
    uint32_t flags;
    NvnVkRenderTier render_tier;
} NvnVkQuirkRegistry;

static inline NvnVkGpuProfile nvn_vk_gpu_profile_make(
    const VkPhysicalDeviceProperties *properties,
    const VkPhysicalDeviceDriverProperties *driver,
    const VkPhysicalDeviceSubgroupProperties *subgroup) {
    NvnVkGpuProfile profile = {0};
    if (properties) {
        profile.api_version = properties->apiVersion;
        profile.vendor_id = properties->vendorID;
        profile.device_id = properties->deviceID;
        profile.driver_version = properties->driverVersion;
    }
    if (driver) profile.driver_id = (uint32_t)driver->driverID;
    if (subgroup) {
        profile.subgroup_size = subgroup->subgroupSize;
        profile.subgroup_stages = subgroup->supportedStages;
        profile.subgroup_operations = subgroup->supportedOperations;
    }
    return profile;
}

static inline NvnVkQuirkRegistry nvn_vk_quirk_registry_resolve(
    const NvnVkGpuProfile *profile) {
    NvnVkQuirkRegistry registry = { .flags = NVN_VK_QUIRK_NONE,
        .render_tier = NVN_VK_RENDER_TIER_FULL };
    if (!profile) return registry;

    if (profile->driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY) {
        registry.flags |= NVN_VK_QUIRK_LOWER_SKIN_UBO;
        registry.render_tier = NVN_VK_RENDER_TIER_COMPAT;
        if ((profile->subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) &&
            (profile->subgroup_operations & VK_SUBGROUP_FEATURE_ARITHMETIC_BIT))
            registry.flags |= NVN_VK_QUIRK_LOWER_COMPUTE_BALLOT;
    }
    return registry;
}

static inline int nvn_vk_quirk_enabled(const NvnVkQuirkRegistry *registry,
    NvnVkQuirk quirk) {
    return registry && (registry->flags & (uint32_t)quirk) != 0;
}

#endif
