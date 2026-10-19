#include <assert.h>
#include <stdio.h>
#include "../../gfx/nvn/nvn_vk_zcull.h"
int main(void) {
    NvnVkZCullRecord a=nvn_vk_zcull_record(53,1920,1080,1,0,1),b=a;
    assert(nvn_vk_zcull_valid(&a,&b));
    for(unsigned i=0;i<16;i++){b=a;b.word[i]^=1;assert(!nvn_vk_zcull_valid(&b,&a));}
    b=nvn_vk_zcull_record(53,1280,720,1,0,1);assert(!nvn_vk_zcull_valid(&b,&a));
    b=nvn_vk_zcull_record(52,1920,1080,1,0,1);assert(!nvn_vk_zcull_valid(&b,&a));
    b=nvn_vk_zcull_record(53,1920,1080,4,0,1);assert(!nvn_vk_zcull_valid(&b,&a));
    b=nvn_vk_zcull_record(53,1920,1080,1,1,1);assert(!nvn_vk_zcull_valid(&b,&a));
    b=nvn_vk_zcull_record(53,1920,1080,1,0,2);assert(!nvn_vk_zcull_valid(&b,&a));
    puts("ZCull record integrity/compatibility passed");return 0;
}
