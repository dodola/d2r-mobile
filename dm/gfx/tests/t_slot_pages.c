/* 将生产位图运算与逐页参考实现比较，覆盖跨字、重叠、读写和端点。
 * Compare production bitmap operations against a per-page reference across words, overlaps, reads/writes, and endpoints.
 */
#include "../nvn/nvn_vk_slot_pages.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint32_t reads[128]={0},writes[128]={0},ref[128]={0};uint32_t state=7;
    for(unsigned round=0;round<20000;round++){
        state=state*1664525u+1013904223u;unsigned a=state%4096;
        state=state*1664525u+1013904223u;unsigned b=state%4096;if(a>b){unsigned t=a;a=b;b=t;}
        if(!(round%17)){memset(reads,0,sizeof reads);memset(writes,0,sizeof writes);memset(ref,0,sizeof ref);}
        nvn_slot_pages_mark(reads,a,b);for(unsigned i=a;i<=b;i++)ref[i>>5]|=1u<<(i&31);
        assert(!memcmp(reads,ref,sizeof ref));
        if(round&1)nvn_slot_pages_mark(writes,a,b);
        state=state*1664525u+1013904223u;a=state%4096;state=state*1664525u+1013904223u;b=state%4096;
        if(a>b){unsigned t=a;a=b;b=t;}
        for(int cpu=0;cpu<2;cpu++){
            unsigned hits=0;int wh=0,actual=0;
            for(unsigned i=a;i<=b;i++){unsigned m=1u<<(i&31);if(writes[i>>5]&m)wh=1;if((writes[i>>5]|(cpu?reads[i>>5]:0))&m)hits++;}
            assert(nvn_slot_pages_hits(cpu?reads:NULL,writes,a,b,&actual)==hits&&actual==wh);
        }
    }
    puts("SLOT_PAGES PASS cases=20000 exact_marks exact_hits read_write_masks");return 0;
}
