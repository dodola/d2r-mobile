#include "../../gfx/nvn/nvn_vk_sampler_policy.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    VkPhysicalDeviceLimits limits={.maxSamplerAnisotropy=8,.maxSamplerLodBias=2};
    VkPhysicalDeviceFeatures enabled={0};
    VkSamplerCreateInfo info={.anisotropyEnable=VK_TRUE,.maxAnisotropy=16,.mipLodBias=4};
    nvn_vk_sampler_limits(&info,&enabled,&limits);
    assert(!info.anisotropyEnable&&info.maxAnisotropy==1&&info.mipLodBias==2);
    enabled.samplerAnisotropy=VK_TRUE;info.anisotropyEnable=VK_TRUE;info.maxAnisotropy=16;info.mipLodBias=-4;
    nvn_vk_sampler_limits(&info,&enabled,&limits);
    assert(info.anisotropyEnable&&info.maxAnisotropy==8&&info.mipLodBias==-2);
    info.maxAnisotropy=4;info.mipLodBias=.5f;
    nvn_vk_sampler_limits(&info,&enabled,&limits);
    assert(info.anisotropyEnable&&info.maxAnisotropy==4&&info.mipLodBias==.5f);
    info.maxAnisotropy=NAN;nvn_vk_sampler_limits(&info,&enabled,&limits);
    assert(!info.anisotropyEnable&&info.maxAnisotropy==1);
    info.magFilter=info.minFilter=VK_FILTER_LINEAR;info.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR;
    assert(nvn_vk_sampler_filter(&info,VK_FORMAT_R32_SFLOAT,0));
    assert(info.magFilter==VK_FILTER_NEAREST&&info.minFilter==VK_FILTER_NEAREST&&info.mipmapMode==VK_SAMPLER_MIPMAP_MODE_NEAREST);
    info.compareEnable=VK_TRUE;info.magFilter=info.minFilter=VK_FILTER_LINEAR;
    assert(!nvn_vk_sampler_filter(&info,VK_FORMAT_D32_SFLOAT,0));
    assert(info.magFilter==VK_FILTER_LINEAR);
    info.compareEnable=VK_FALSE;info.borderColor=VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    assert(nvn_vk_sampler_filter(&info,VK_FORMAT_R32_UINT,0));
    assert(info.borderColor==VK_BORDER_COLOR_INT_OPAQUE_WHITE&&info.magFilter==VK_FILTER_NEAREST);
    info.magFilter=info.minFilter=VK_FILTER_LINEAR;
    assert(!nvn_vk_sampler_filter(&info,VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT));
    assert(info.magFilter==VK_FILTER_LINEAR);
    puts("SAMPLER_POLICY passed=1");return 0;
}
