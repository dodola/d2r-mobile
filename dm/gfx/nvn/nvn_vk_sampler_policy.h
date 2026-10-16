#ifndef DM_NVN_VK_SAMPLER_POLICY_H
#define DM_NVN_VK_SAMPLER_POLICY_H
#include <math.h>
#include <vulkan/vulkan.h>
static inline int nvn_vk_format_integer(VkFormat f) {
    switch(f){
    case VK_FORMAT_R8_UINT:case VK_FORMAT_R8_SINT:case VK_FORMAT_R8G8_UINT:case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8_UINT:case VK_FORMAT_R8G8B8_SINT:case VK_FORMAT_R8G8B8A8_UINT:case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_B8G8R8_UINT:case VK_FORMAT_B8G8R8_SINT:case VK_FORMAT_B8G8R8A8_UINT:case VK_FORMAT_B8G8R8A8_SINT:
    case VK_FORMAT_R16_UINT:case VK_FORMAT_R16_SINT:case VK_FORMAT_R16G16_UINT:case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16_UINT:case VK_FORMAT_R16G16B16_SINT:case VK_FORMAT_R16G16B16A16_UINT:case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_UINT:case VK_FORMAT_R32_SINT:case VK_FORMAT_R32G32_UINT:case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_UINT:case VK_FORMAT_R32G32B32_SINT:case VK_FORMAT_R32G32B32A32_UINT:case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2R10G10B10_UINT_PACK32:case VK_FORMAT_A2B10G10R10_UINT_PACK32:return 1;
    default:return 0;
    }
}
/* Comparison sampling has separate rules: lack of ordinary linear filtering
 * alone does not imply depth comparison sampling must become nearest. */
static inline int nvn_vk_sampler_filter(VkSamplerCreateInfo *info,VkFormat format,VkFormatFeatureFlags features) {
    int integer=nvn_vk_format_integer(format),changed=0;
    if(integer||(!info->compareEnable&&!(features&VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))){
        changed=info->magFilter!=VK_FILTER_NEAREST||info->minFilter!=VK_FILTER_NEAREST||
            info->mipmapMode!=VK_SAMPLER_MIPMAP_MODE_NEAREST||info->anisotropyEnable;
        info->magFilter=info->minFilter=VK_FILTER_NEAREST;info->mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info->anisotropyEnable=VK_FALSE;info->maxAnisotropy=1.f;
    }
    if(integer){
        switch(info->borderColor){
        case VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK:info->borderColor=VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;changed=1;break;
        case VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK:info->borderColor=VK_BORDER_COLOR_INT_OPAQUE_BLACK;changed=1;break;
        case VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE:info->borderColor=VK_BORDER_COLOR_INT_OPAQUE_WHITE;changed=1;break;
        default:break;
        }
    }
    return changed;
}
/* NVN settings may exceed host limits. Disable unsupported optional quality;
 * do not submit invalid VkSamplerCreateInfo values to the driver. */
static inline void nvn_vk_sampler_limits(VkSamplerCreateInfo *info,
    const VkPhysicalDeviceFeatures *enabled,const VkPhysicalDeviceLimits *limits) {
    if(!enabled->samplerAnisotropy||!isfinite(info->maxAnisotropy)||info->maxAnisotropy<=1.f){
        info->anisotropyEnable=VK_FALSE;info->maxAnisotropy=1.f;
    }else if(info->maxAnisotropy>limits->maxSamplerAnisotropy)
        info->maxAnisotropy=limits->maxSamplerAnisotropy;
    if(info->mipLodBias>limits->maxSamplerLodBias)info->mipLodBias=limits->maxSamplerLodBias;
    if(info->mipLodBias< -limits->maxSamplerLodBias)info->mipLodBias= -limits->maxSamplerLodBias;
}
#endif
