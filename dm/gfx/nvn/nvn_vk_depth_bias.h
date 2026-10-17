/* NVN polygon offset maps to Vulkan's rasterization depth bias. The FILL
 * enable bit alone applies while replay supports filled polygons. Values are
 * stored independently of the enable state, just like the NVN command state. */
#ifndef NVN_VK_DEPTH_BIAS_H
#define NVN_VK_DEPTH_BIAS_H
#include <math.h>
#include <stdint.h>

typedef struct {
    uint32_t enabled;
    float constant, slope, clamp;
} NvnVkDepthBias;

enum { NVN_VK_DEPTH_BIAS_OK, NVN_VK_DEPTH_BIAS_INVALID, NVN_VK_DEPTH_BIAS_CLAMP_UNSUPPORTED };
static inline int nvn_vk_depth_bias(uint32_t enables,float factor,float units,float clamp,
                                   int clamp_supported,NvnVkDepthBias *out) {
    *out=(NvnVkDepthBias){0};
    if(!(enables&4))return NVN_VK_DEPTH_BIAS_OK;
    if(!isfinite(factor)||!isfinite(units)||!isfinite(clamp))return NVN_VK_DEPTH_BIAS_INVALID;
    if(clamp!=0&&!clamp_supported)return NVN_VK_DEPTH_BIAS_CLAMP_UNSUPPORTED;
    out->enabled=1;out->constant=units;out->slope=factor;out->clamp=clamp;
    return NVN_VK_DEPTH_BIAS_OK;
}
#endif
