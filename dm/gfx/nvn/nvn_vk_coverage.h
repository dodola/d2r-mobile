/* Format-aware positive controls for attachment coverage diagnostics.
 * Unsupported formats cannot contribute evidence. The packed word is the
 * exact transfer representation of the value passed to vkCmdBeginRenderPass. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

static inline int nvn_cov_sentinel(VkFormat format,VkClearValue *clear,uint32_t *word) {
    memset(clear,0,sizeof *clear);
    switch(format){
    case VK_FORMAT_R8G8B8A8_UNORM:case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM:case VK_FORMAT_B8G8R8A8_SRGB:
        clear->color.float32[0]=clear->color.float32[2]=clear->color.float32[3]=1.f;
        *word=0xffff00ffu;return 1;
    case VK_FORMAT_R32_UINT:
        clear->color.uint32[0]=*word=0xdeadbeefu;return 1;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        clear->color.float32[0]=clear->color.float32[2]=1.f;
        *word=0x780003c0u;return 1;
    default:return 0;
    }
}
static inline uint64_t nvn_cov_uncovered(const void *data,uint64_t pixels,uint32_t sentinel) {
    const uint8_t *p=data;uint64_t count=0;
    for(uint64_t i=0;i<pixels;i++){uint32_t word;memcpy(&word,p+4*i,4);count+=word==sentinel;}
    return count;
}
