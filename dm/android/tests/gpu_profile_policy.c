/* Host-side policy test.  It exercises the profile/registry without creating
 * a Vulkan instance, so it is safe to run on build machines without a GPU. */
#include "../../gfx/nvn/nvn_vk_profile.h"

#define CHECK(value) do { if (!(value)) return __LINE__; } while (0)

int main(void) {
    VkPhysicalDeviceProperties properties = {0};
    VkPhysicalDeviceDriverProperties driver = {0};
    VkPhysicalDeviceSubgroupProperties subgroup = {0};
    driver.driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    subgroup.supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    subgroup.supportedOperations = VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    NvnVkGpuProfile profile = nvn_vk_gpu_profile_make(&properties, &driver, &subgroup);
    NvnVkQuirkRegistry registry = nvn_vk_quirk_registry_resolve(&profile);
    CHECK(nvn_vk_quirk_enabled(&registry, NVN_VK_QUIRK_LOWER_COMPUTE_BALLOT));
    CHECK(nvn_vk_quirk_enabled(&registry, NVN_VK_QUIRK_LOWER_SKIN_UBO));
    CHECK(registry.render_tier == NVN_VK_RENDER_TIER_COMPAT);

    subgroup.supportedOperations = 0;
    profile = nvn_vk_gpu_profile_make(&properties, &driver, &subgroup);
    registry = nvn_vk_quirk_registry_resolve(&profile);
    CHECK(!nvn_vk_quirk_enabled(&registry, NVN_VK_QUIRK_LOWER_COMPUTE_BALLOT));
    CHECK(nvn_vk_quirk_enabled(&registry, NVN_VK_QUIRK_LOWER_SKIN_UBO));

    driver.driverID = VK_DRIVER_ID_MESA_TURNIP;
    profile = nvn_vk_gpu_profile_make(&properties, &driver, &subgroup);
    registry = nvn_vk_quirk_registry_resolve(&profile);
    CHECK(registry.flags == NVN_VK_QUIRK_NONE);
    CHECK(registry.render_tier == NVN_VK_RENDER_TIER_FULL);
    return 0;
}
