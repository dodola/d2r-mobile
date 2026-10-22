/* 用真实脏页跟踪验证轮转槽的增量快照和在途版本不变性。
 * Verify rotating incremental snapshots and immutable in-flight versions against the real dirty-page tracker.
 */
#include "../nvn/nvn_vk_snapshot_patch.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p){(void)p;return NULL;}
void crash_install(void);
static unsigned checks;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"snapshot check failed line %d: %s\n",__LINE__,#x);return 1;}} while(0)
int main(void){
    crash_install();
    for(unsigned round=0;round<300;round++){
        PoolUploadRange writes[16],range={17,111},span;uint8_t wanted[160]={0},got[160]={0};
        for(unsigned i=0;i<16;i++){
            writes[i]=(PoolUploadRange){(round*13+i*31)%140,(round+i*7)%20};
            for(uint64_t x=writes[i].addr;x<writes[i].addr+writes[i].size;x++)if(x>=range.addr&&x<range.addr+range.size)wanted[x]=1;
        }
        uint64_t cursor=range.addr;
        while(nvn_vk_snapshot_next_write(range,writes,16,&cursor,&span)){
            CHECK(span.size&&span.addr>=range.addr&&span.addr+span.size<=range.addr+range.size);
            for(uint64_t x=span.addr;x<span.addr+span.size;x++){CHECK(!got[x]);got[x]=1;}
        }
        CHECK(!memcmp(wanted,got,sizeof wanted));
    }
    const size_t size=64*4096,skip=37,len=size-91;
    uint8_t *guest=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    CHECK(guest!=MAP_FAILED);
    for(size_t i=0;i<size;i++)guest[i]=(uint8_t)(i*17+3);
    nvn_dirty_add_range(guest,size);
    uint8_t *slot[3],*expected[3];uint64_t stamp[3]={0};
    for(unsigned i=0;i<3;i++){slot[i]=malloc(len);expected[i]=malloc(len);CHECK(slot[i]&&expected[i]);memset(slot[i],0,len);memset(expected[i],0,len);}
    uint64_t incremental=0;
    for(unsigned round=0;round<300;round++){
        unsigned at=round%3;
        uint64_t next=nvn_dirty_now();nvn_dirty_protect(guest+skip,len);
        uint64_t copied=nvn_vk_snapshot_patch(slot[at],guest+skip,len,stamp[at]);
        CHECK(copied<=len);CHECK(!memcmp(slot[at],guest+skip,len));
        if(round<3)CHECK(copied==len);else{CHECK(copied<len);incremental+=copied;}
        stamp[at]=next;memcpy(expected[at],slot[at],len);
        for(unsigned i=0;i<3;i++)CHECK(!memcmp(slot[i],expected[i],len));
        CHECK(nvn_vk_snapshot_patch(slot[at],guest+skip,len,stamp[at])==0);
        for(unsigned j=0;j<2;j++){
            size_t pos=((round*19+j*23)%64)*4096+128;
            if(j)dm_guest_write_prepare(guest+pos,1);
            guest[pos]^=(uint8_t)(round+1);
        }
        for(unsigned i=0;i<3;i++)CHECK(!memcmp(slot[i],expected[i],len));
    }
    CHECK(incremental<300*len/2);
    uint8_t untracked[13],destination[13];memset(untracked,0x5a,sizeof untracked);
    CHECK(nvn_vk_snapshot_patch(destination,untracked,sizeof untracked,1)==sizeof untracked);
    CHECK(!memcmp(destination,untracked,sizeof untracked));
    nvn_dirty_remove_range(guest);munmap(guest,size);
    for(unsigned i=0;i<3;i++){free(slot[i]);free(expected[i]);}
    printf("SNAPSHOT_PATCH checks=%u failures=0 rotating_slots=3 incremental_bytes=%llu\n",checks,(unsigned long long)incremental);
    return 0;
}
