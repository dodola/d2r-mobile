#include "../relr.h"
#include <assert.h>
#include <stdio.h>

static uint64_t read64(const uint8_t *p) { uint64_t v;memcpy(&v,p,8);return v; }
int main(void) {
    uint8_t image[2048]={0};size_t count=0;
    /* Direct 128; bitmap relocates 136, 152, 632; next bitmap 640;
     * direct reset to an even unaligned address 1026. */
    const uint64_t words[]={128,1|(UINT64_C(1)<<1)|(UINT64_C(1)<<3)|(UINT64_C(1)<<63),3,1026};
    const size_t offsets[]={128,136,152,632,640,1026};
    uint64_t addend=0x1234;
    for(size_t i=0;i<sizeof offsets/sizeof *offsets;i++)memcpy(image+offsets[i],&addend,8);
    assert(!dm_android_relr_apply(image,sizeof image,128,sizeof image,words,4,&count));
    assert(count==6);
    for(size_t i=0;i<sizeof offsets/sizeof *offsets;i++)
        assert(read64(image+offsets[i])==(uint64_t)(uintptr_t)image+addend);
    assert(read64(image+144)==0); /* A zero bitmap bit leaves the slot untouched. */
    uint8_t before[sizeof image];memcpy(before,image,sizeof image);
    const uint64_t bad_streams[][2]={{3,0},{128,2042},{128,UINT64_MAX-1},{128,120},{2040,3}};
    for(size_t i=0;i<sizeof bad_streams/sizeof *bad_streams;i++) {
        assert(dm_android_relr_apply(image,sizeof image,128,sizeof image,bad_streams[i],2,&count));
        assert(!memcmp(before,image,sizeof image)); /* No partial writes on error. */
    }
    const uint64_t protected_text[]={126};
    assert(dm_android_relr_apply(image,sizeof image,128,sizeof image,protected_text,1,&count));
    assert(!dm_android_relr_apply(image,sizeof image,128,sizeof image,NULL,0,&count) && count==0);
    puts("ELF64 RELR address/bitmap/bounds/atomic-rejection checks passed");
    return 0;
}
