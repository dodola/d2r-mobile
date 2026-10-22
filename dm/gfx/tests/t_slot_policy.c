/* 默认策略跨设备一致、覆盖优先级和回退组合测试。
 * Test consistent defaults across devices, override priority and rollback combinations.
 */
#include "../nvn/nvn_vk_slot_policy.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"slot policy failed: %s\n",#x);return 1;}}while(0)
int main(void){
    const char *soc[]={"SM8750","SM7325","MT8795Z","SM8850","",NULL};
    const char *gpu[]={"Adreno (TM) 830","Adreno (TM) 642L","Mali-G610 MC6","Adreno (TM) 840",NULL};
    unsigned checks=0;
    for(unsigned s=0;s<6;s++)for(unsigned g=0;g<5;g++)for(int driver=0;driver<2;driver++){
        NvnSlotPolicy p=nvn_vk_slot_policy(soc[s],gpu[g],driver,NULL,0,0);
        CHECK(p.slots==2&&p.automatic&&p.publication&&p.uniforms&&p.buffers);checks++;
    }
    for(unsigned disable=1;disable<8;disable++){
        NvnSlotPolicy p=nvn_vk_slot_policy(soc[0],gpu[0],1,NULL,0,disable);CHECK(p.slots==1&&!p.automatic&&!p.uniforms&&!p.buffers);checks++;
    }
    NvnSlotPolicy p=nvn_vk_slot_policy(soc[0],gpu[0],1,"1",0,0);CHECK(p.slots==1&&p.publication&&!p.uniforms&&!p.buffers);
    p=nvn_vk_slot_policy(soc[1],gpu[1],1,"2",0,0);CHECK(p.slots==2&&p.automatic&&p.buffers);
    p=nvn_vk_slot_policy(soc[0],gpu[0],1,"2",7,1);CHECK(p.slots==2&&!p.publication&&!p.uniforms&&!p.buffers);
    p=nvn_vk_slot_policy(soc[0],gpu[0],1,"2",7,2);CHECK(p.publication&&!p.uniforms&&!p.buffers);
    p=nvn_vk_slot_policy(soc[0],gpu[0],1,"2",7,4);CHECK(p.uniforms&&!p.buffers);
    p=nvn_vk_slot_policy(soc[0],gpu[0],1,"99",7,0);CHECK(p.slots==4);
    p=nvn_vk_slot_policy(soc[0],gpu[0],1,"bad",7,0);CHECK(p.slots==1);
    printf("SLOT_POLICY PASS device_combinations=%u rollback override dependency\n",checks);return 0;
}
