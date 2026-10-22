/* 只在已验收的 SoC 和驱动组合默认启用双槽，逐步开关仍可回退。
 * Default to two slots only for the accepted SoC/driver combination, with independent rollback switches.
 */
#ifndef NVN_VK_SLOT_POLICY_H
#define NVN_VK_SLOT_POLICY_H
#include <string.h>
#include <stdlib.h>
enum {NVN_SLOT_PUBLICATION=1,NVN_SLOT_UNIFORMS=2,NVN_SLOT_BUFFERS=4};
typedef struct {int slots,publication,uniforms,buffers,automatic;} NvnSlotPolicy;
static inline NvnSlotPolicy nvn_vk_slot_policy(const char *soc,const char *gpu,int qualcomm_system,
        const char *requested,unsigned enable,unsigned disable) {
    int automatic=soc&&gpu&&qualcomm_system&&!strcmp(soc,"SM8750")&&!strcmp(gpu,"Adreno (TM) 830")&&!disable;
    int slots=requested?atoi(requested):automatic?2:1;if(slots<1)slots=1;if(slots>4)slots=4;
    unsigned enabled=(enable|(automatic?7u:0u))&~disable;
    int publication=(enabled&NVN_SLOT_PUBLICATION)!=0;
    int uniforms=publication&&slots>1&&(enabled&NVN_SLOT_UNIFORMS)!=0;
    return (NvnSlotPolicy){slots,publication,uniforms,uniforms&&(enabled&NVN_SLOT_BUFFERS)!=0,automatic};
}
#endif
