#include "../../gfx/nvn/nvn_vk_unused_distance.h"
#include <assert.h>
#include <stdio.h>
#define W(op,n) ((uint32_t)(n)<<16|(op))
static const uint32_t module[]={0x07230203,0x10300,0,64,0,
    W(17,2),1,W(17,2),32,W(17,2),33,W(14,3),0,1,
    W(15,6),0,40,0x6e69616d,0,20,
    W(6,4),10,0,0,W(6,4),10,1,0,W(6,4),10,2,0,W(6,4),10,3,0,
    W(71,3),10,2,
    W(72,5),10,0,11,3,W(72,5),10,1,11,0,W(72,5),10,2,11,4,W(72,5),10,3,11,1,
    W(22,3),1,32,W(23,4),2,1,4,W(21,4),3,32,0,W(43,4),3,7,1,
    W(28,4),4,1,7,W(30,6),10,4,2,4,1,
    W(32,4),11,3,10,W(32,4),12,3,2,W(32,4),17,3,1,
    W(43,4),3,13,0,W(43,4),3,14,1,W(43,4),3,15,2,W(43,4),3,16,3,
    W(43,4),1,8,0,W(44,7),2,21,8,8,8,8,
    W(19,2),22,W(33,3),23,22,W(59,4),11,20,3,
    W(54,5),22,40,0,23,W(248,2),41,
    W(65,5),12,30,20,14,W(62,3),30,21,
    W(65,5),17,31,20,16,W(62,3),31,8,W(253,1),W(56,1)};
static size_t find(uint32_t *m,size_t n,unsigned op,unsigned occurrence){
    for(size_t at=5;at<n;at+=m[at]>>16)if((m[at]&65535)==op&&!occurrence--)return at;
    assert(!"instruction not found");return 0;
}
static void unchanged(const uint32_t *m,size_t n){
    size_t count=0;uint32_t removed=123;const char *error=NULL;
    uint32_t *out=nvn_vk_unused_distance(m,n,&count,&removed,&error);
    assert(out&&!error&&!removed&&count==n&&!memcmp(m,out,n*4));free(out);
}
static void unit_tests(void){
    size_t n=sizeof module/4,count=0;uint32_t removed=0;const char *error=NULL;
    uint32_t *out=nvn_vk_unused_distance(module,n,&count,&removed,&error);
    assert(out&&!error&&removed==3&&out[3]==66);
    size_t at=find(out,count,30,0);assert((out[at]>>16)==4&&out[at+2]==2&&out[at+3]==1);
    unsigned capabilities=0,members=0,names=0;for(size_t i=5;i<count;i+=out[i]>>16){
        unsigned op=out[i]&65535;capabilities+=op==17;members+=op==72;names+=op==6;
        if(op==43&&out[i+2]==14)assert(out[i+3]==1); /* Never mutate shared original constants. */
    }
    assert(capabilities==1&&members==2&&names==2);
    at=find(out,count,65,0);assert(out[at+4]==64);at=find(out,count,65,1);assert(out[at+4]==65);
    at=find(out,count,72,0);assert(out[at+2]==0&&out[at+4]==0);
    at=find(out,count,72,1);assert(out[at+2]==1&&out[at+4]==1);free(out);
    uint32_t changed[512];memcpy(changed,module,sizeof module);
    /* A real access preserves ClipDistance; CullDistance alone is removed. */
    at=find(changed,n,253,0);const uint32_t clip[]={W(65,6),17,32,20,13,13,W(62,3),32,8};
    memmove(changed+at+sizeof clip/4,changed+at,(n-at)*4);memcpy(changed+at,clip,sizeof clip);
    out=nvn_vk_unused_distance(changed,n+sizeof clip/4,&count,&removed,&error);assert(out&&!error&&removed==2);
    at=find(out,count,30,0);assert((out[at]>>16)==5&&out[at+2]==4&&out[at+3]==2&&out[at+4]==1);
    unsigned clipcaps=0,cullcaps=0;for(size_t i=5;i<count;i+=out[i]>>16)if((out[i]&65535)==17){clipcaps+=out[i+1]==32;cullcaps+=out[i+1]==33;}
    assert(clipcaps==1&&!cullcaps);free(out);
    memcpy(changed,module,sizeof module);at=find(changed,n,65,0);changed[at+4]=8;unchanged(changed,n); /* noninteger member index */
    memcpy(changed,module,sizeof module);at=find(changed,n,65,0);changed[at]=W(83,5);unchanged(changed,n); /* root pointer escape */
    memcpy(changed,module,sizeof module);at=find(changed,n,32,0);changed[at+2]=1;unchanged(changed,n); /* Input block */
    memcpy(changed,module,sizeof module);at=find(changed,n,30,0);changed[at+2]=10;unchanged(changed,n); /* nested aggregate */
    memcpy(changed,module,sizeof module);at=find(changed,n,72,0);changed[at+4]=0;
    at=find(changed,n,72,2);changed[at+4]=1;unchanged(changed,n); /* no distance declarations */
    memcpy(changed,module,sizeof module);changed[5]=0;
    out=nvn_vk_unused_distance(changed,n,&count,&removed,&error);assert(!out&&!count&&!removed&&error);
    for(size_t cut=0;cut<5;cut++){out=nvn_vk_unused_distance(module,cut,&count,&removed,&error);assert(!out&&!count&&!removed&&error);}
    for(size_t i=5;i<n;i+=module[i]>>16){memcpy(changed,module,sizeof module);changed[i]&=65535;out=nvn_vk_unused_distance(changed,n,&count,&removed,&error);assert(!out&&!count&&!removed&&error);}
    puts("unused distance: interleaved member remap, shared constants, names/decorations, capabilities, active clip, opaque fallback and bounds checks passed");
}
int main(int argc,char **argv){
    if(argc==1){unit_tests();return 0;}if(argc!=3)return 2;
    if(!strcmp(argv[1],"--fixture")){
        FILE *fixture=fopen(argv[2],"wb");if(!fixture)return 3;
        int ok=fwrite(module,1,sizeof module,fixture)==sizeof module;
        if(fclose(fixture))ok=0;
        return ok?0:3;
    }
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END)){fclose(f);return 3;}long bytes=ftell(f);rewind(f);
    if(bytes<20||bytes%4){fclose(f);return 3;}
    uint32_t *input=malloc((size_t)bytes);if(!input){fclose(f);return 3;}
    if(fread(input,1,(size_t)bytes,f)!=(size_t)bytes){free(input);fclose(f);return 3;}fclose(f);
    size_t words=0;uint32_t removed=0;const char *error=NULL;
    uint32_t *output=nvn_vk_unused_distance(input,(size_t)bytes/4,&words,&removed,&error);free(input);
    if(!output){fprintf(stderr,"%s\n",error);return 4;}
    f=fopen(argv[2],"wb");if(!f){free(output);return 3;}
    int ok=fwrite(output,4,words,f)==words;if(fclose(f))ok=0;free(output);if(!ok)return 3;
    printf("{\"removed_mask\":%u,\"words\":%zu}\n",removed,words);return 0;
}
