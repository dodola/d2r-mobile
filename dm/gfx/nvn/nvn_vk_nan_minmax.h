/* Maxwell 的浮点最小/最大/饱和忽略单个 NaN；GLSL FMin/FMax/FClamp 不保证这一点。
 * 改写思路来自 jhaizhou-ops/d2r-mobile 3970f8b；先验证指令边界和参数个数，再原位改扩展操作码。
 * Maxwell floating min/max/saturation ignore a single NaN; GLSL FMin/FMax/FClamp do not guarantee this.
 * Based on jhaizhou-ops/d2r-mobile 3970f8b; validate instruction bounds and arity before changing extended opcodes.
 */
#ifndef DM_NVN_VK_NAN_MINMAX_H
#define DM_NVN_VK_NAN_MINMAX_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { size_t min, max, clamp; } NvnVkNanMinmaxStats;

/* 失败时模块和计数保持不变/零；这里只验证改写所需结构，不代替完整 SPIR-V 校验器。
 * On failure the module is unchanged and counts are zero; validate rewrite structure, not full SPIR-V semantics.
 */
static inline int nvn_vk_nan_minmax(uint32_t *w,size_t count,NvnVkNanMinmaxStats *stats) {
    if(stats)memset(stats,0,sizeof *stats);
    if(!w||count<5||w[0]!=0x07230203u||!w[3]||w[3]>1000000u)return 0;
    uint8_t *glsl=calloc(w[3],1);if(!glsl)return 0;
    NvnVkNanMinmaxStats found={0};
    for(size_t i=5;i<count;){
        uint32_t n=w[i]>>16,op=w[i]&0xffffu;
        if(!n||n>count-i)goto bad;
        if(op==11){
            if(n<3||!w[i+1]||w[i+1]>=w[3])goto bad;
            const char *name=(const char *)&w[i+2];size_t bytes=(size_t)(n-2)*4;
            const char *end=memchr(name,0,bytes);if(!end)goto bad;
            if((size_t)(end-name)==12&&!memcmp(name,"GLSL.std.450",12))glsl[w[i+1]]=1;
        }
        if(op==12&&(n<5||!w[i+3]||w[i+3]>=w[3]))goto bad;
        i+=n;
    }
    for(size_t i=5;i<count;i+=w[i]>>16){
        if((w[i]&0xffffu)!=12||!glsl[w[i+3]])continue;
        uint32_t e=w[i+4],n=w[i]>>16;
        if(e==37||e==40||e==43){
            if(n!=(e==43?8u:7u))goto bad;
            if(e==37)found.min++;else if(e==40)found.max++;else found.clamp++;
        }
    }
    for(size_t i=5;i<count;i+=w[i]>>16){
        if((w[i]&0xffffu)!=12||!glsl[w[i+3]])continue;
        uint32_t *e=&w[i+4];if(*e==37)*e=79;else if(*e==40)*e=80;else if(*e==43)*e=81;
    }
    free(glsl);if(stats)*stats=found;return 1;
bad:
    free(glsl);return 0;
}
#endif
