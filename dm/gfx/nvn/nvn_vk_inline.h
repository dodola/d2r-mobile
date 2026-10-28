/* Adreno 750 运行时内联接口，改编自 upstream 2428e5b；保持本地 SPIR-V 资源契约。
 * Runtime inlining for Adreno 750, adapted from upstream 2428e5b; preserves the local SPIR-V resource contract. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define DM_VK_INLINE_REVISION 1u
static inline int dm_vk_adreno_model(const char *name) {
    if(!name)return 0;
    const char *p=strstr(name,"Adreno");if(!p)p=strstr(name,"adreno");if(!p)return 0;p+=6;
    while(*p&&(*p<'0'||*p>'9'))p++;
    int n=0,digits=0;while(*p>='0'&&*p<='9'&&digits<4){n=n*10+*p++-'0';digits++;}
    return digits==3?n:0;
}
static inline int dm_vk_inline_default(uint32_t vendor,const char *name) {
    int n=dm_vk_adreno_model(name);return vendor==0x5143&&n>=750&&n<800;
}
#ifdef __cplusplus
extern "C" {
#endif
/* 返回 1 表示内联，0 表示无需改写，-1 表示模块或优化失败。成功结果由调用者 free。
 * Returns 1 for inlining, 0 when unchanged, -1 for malformed input or optimization failure. Caller frees successful output. */
int dm_vk_inline_helpers(const uint32_t *words,size_t count,uint32_t **out,size_t *out_count);
int dm_vk_subgroup_helper(const uint32_t *words,size_t count);
#ifdef __cplusplus
}
#endif
