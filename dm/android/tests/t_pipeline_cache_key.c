/* Host-side regression test for the Vulkan pipeline-cache identity. */
#include "../../gfx/nvn/nvn_vk_pipeline_cache_key.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    VkPhysicalDeviceProperties properties;
    memset(&properties, 0, sizeof properties);
    properties.apiVersion = VK_API_VERSION_1_1;
    properties.vendorID = 0x5143;
    properties.deviceID = 0x1234;
    properties.driverVersion = 0x01020304;
    for (unsigned i = 0; i < VK_UUID_SIZE; ++i)
        properties.pipelineCacheUUID[i] = (uint8_t)(i + 1);

    VkPhysicalDeviceDriverProperties driver;
    memset(&driver, 0, sizeof driver);
    driver.driverID = VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    VkPhysicalDeviceSubgroupProperties subgroup;
    memset(&subgroup, 0, sizeof subgroup);
    subgroup.subgroupSize = 64;
    subgroup.supportedStages = VK_SHADER_STAGE_COMPUTE_BIT;
    subgroup.supportedOperations = VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    NvnVkGpuProfile profile = nvn_vk_gpu_profile_make(&properties, &driver, &subgroup);
    NvnVkQuirkRegistry registry = nvn_vk_quirk_registry_resolve(&profile);

    NvnVkPipelineCacheKey a = nvn_vk_pipeline_cache_key_make(&properties, &profile, &registry);
    NvnVkPipelineCacheKey b = nvn_vk_pipeline_cache_key_make(&properties, &profile, &registry);
    assert(nvn_vk_pipeline_cache_key_equal(&a, &b));
    /* NaN 开关隔离两套缓存，重复设置保持幂等。
     * The NaN switch separates caches, and repeated setting is idempotent. */
    nvn_vk_pipeline_cache_key_nan_minmax(&b,1);
    assert(!nvn_vk_pipeline_cache_key_equal(&a,&b));
    NvnVkPipelineCacheKey enabled=b;
    nvn_vk_pipeline_cache_key_nan_minmax(&b,1);
    assert(nvn_vk_pipeline_cache_key_equal(&enabled,&b));
    nvn_vk_pipeline_cache_key_nan_minmax(&b,0);
    assert(nvn_vk_pipeline_cache_key_equal(&a,&b));
    assert(a.quirk_flags & NVN_VK_QUIRK_LOWER_SKIN_UBO);
    assert(a.quirk_flags & NVN_VK_QUIRK_LOWER_COMPUTE_BALLOT);

    /* Every compatibility-policy change must invalidate the driver blob. */
    b.quirk_flags ^= NVN_VK_QUIRK_LOWER_SKIN_UBO;
    assert(!nvn_vk_pipeline_cache_key_equal(&a, &b));
    b = a;
    b.pipeline_cache_uuid[0] ^= 1;
    assert(!nvn_vk_pipeline_cache_key_equal(&a, &b));
    printf("NVN_PIPELINE_CACHE_KEY passed=1 size=%zu quirk_flags=%#x\n",
        sizeof a, a.quirk_flags);
    return 0;
}
