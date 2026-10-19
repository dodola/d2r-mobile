/* Vulkan owns HiZ with each VkImage. NVN's external ZCull buffer therefore
 * serializes a conservative (no external hierarchy) state, not depth pixels or
 * NVIDIA hardware metadata. Restoring it orders prior work and retains the
 * destination image's authoritative depth/stencil contents. */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t word[16]; } NvnVkZCullRecord;
_Static_assert(sizeof(NvnVkZCullRecord)==64,"ZCull storage ABI");
static inline uint32_t nvn_vk_zcull_checksum(const NvnVkZCullRecord *r) {
    uint32_t h=2166136261u;
    for(unsigned i=0;i<15;i++)h=(h^r->word[i])*16777619u;
    return h;
}
static inline NvnVkZCullRecord nvn_vk_zcull_record(uint32_t format,uint32_t width,uint32_t height,
                                                 uint32_t samples,uint32_t level,uint32_t layers) {
    NvnVkZCullRecord r={{0x435a4d44,1,sizeof(r),format,width,height,samples,level,layers}};
    r.word[15]=nvn_vk_zcull_checksum(&r);return r;
}
static inline int nvn_vk_zcull_valid(const NvnVkZCullRecord *r,const NvnVkZCullRecord *expected) {
    return r->word[15]==nvn_vk_zcull_checksum(r)&&!memcmp(r,expected,sizeof(*r));
}
