#include "../../gfx/nvn/nvn_vk_image_type.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define INS(n,op) (((uint32_t)(n)<<16)|(op))
typedef struct {uint32_t words[2048];size_t size;} Module;
static unsigned checks;
static Module image(unsigned dim,unsigned depth,unsigned arrayed,unsigned ms,unsigned sampled) {
    const uint32_t words[]={0x07230203,0x10300,0,512,0,
        INS(3,22),1,32,
        INS(9,25),2,1,dim,depth,arrayed,ms,sampled,0,
        INS(3,27),3,2,INS(4,32),4,0,3,
        INS(4,71),5,34,1,INS(4,71),5,33,2,
        INS(4,59),4,5,0};
    Module m={{0},sizeof words/4};memcpy(m.words,words,sizeof words);return m;
}
static void append(Module *m,const uint32_t *words,size_t count) {
    assert(m->size+count<=sizeof m->words/4);
    memcpy(m->words+m->size,words,count*4);m->size+=count;
}
#define APPEND(m,...) do {const uint32_t a[]={__VA_ARGS__};append(&(m),a,sizeof a/4);}while(0)
static void reject(Module m) {
    NvnVkImageType records[64];size_t count=999;const char *error=NULL;
    assert(!nvn_vk_image_types(m.words,m.size,records,&count,&error));
    assert(!count&&error);checks++;
}
static NvnVkImageType accept(Module m,size_t expected) {
    uint32_t before[2048];memcpy(before,m.words,m.size*4);
    NvnVkImageType records[64]={{0}};size_t count=999;const char *error="old";
    assert(nvn_vk_image_types(m.words,m.size,records,&count,&error));
    assert(count==expected&&!error&&!memcmp(before,m.words,m.size*4));checks++;
    return records[0];
}
static void unit_tests(void) {
    for(unsigned dim=0;dim<=6;dim++)for(unsigned array=0;array<2;array++)
    for(unsigned depth=0;depth<3;depth++)for(unsigned ms=0;ms<2;ms++)
    for(unsigned sampled=0;sampled<3;sampled++){
        NvnVkImageType r=accept(image(dim,depth,array,ms,sampled),1);
        assert(r.variable_id==5&&r.image_type_id==2&&r.sampled_image_type_id==3);
        assert(r.descriptor_set==1&&r.binding==2&&r.sampled_type_id==1);
        assert(r.dim==dim&&r.depth==depth&&r.arrayed==array&&r.multisampled==ms);
        assert(r.sampled==sampled&&!r.image_format&&r.access_qualifier==UINT32_MAX);
    }
    Module base=image(1,1,1,0,1),m=base;
    m.words[23]=2; /* Pointer directly to OpTypeImage, separate image. */
    assert(!accept(m,1).sampled_image_type_id);
    APPEND(m,INS(2,26),6,INS(4,32),7,0,6,INS(4,59),7,8,0);
    accept(m,1); /* A separate sampler does not become an image. */
    m=(Module){{0x07230203,0x10300,0,512,0},5};accept(m,0);
    APPEND(m,INS(2,73),1);accept(m,0); /* Unused group, no images. */
    m=base;m.words[19]=99;reject(m); /* Unknown sampled-image target. */
    m=base;m.words[23]=99;reject(m); /* Unknown pointer target. */
    m=base;m.words[22]=1;reject(m); /* Wrong pointer storage. */
    m=base;m.words[30]=99;reject(m); /* Missing binding. */
    m=base;m.words[26]=99;reject(m); /* Missing set. */
    m=base;APPEND(m,INS(4,71),5,33,2);reject(m);
    m=base;APPEND(m,INS(2,73),6);reject(m);
    m=base;APPEND(m,INS(4,332),5,33,1);reject(m);
    m=base;APPEND(m,INS(4,28),6,3,1);m.words[23]=6;reject(m);
    m=base;APPEND(m,INS(3,29),6,3);m.words[23]=6;reject(m);
    m=base;APPEND(m,INS(4,71),6,34,1,INS(4,71),6,33,2,INS(4,59),4,6,0);reject(m);
    m=base;APPEND(m,INS(4,71),6,34,2,INS(4,71),6,33,2,INS(4,59),4,6,0);accept(m,2);
    m=base;APPEND(m,INS(4,71),6,34,1,INS(4,71),6,33,3,INS(4,59),4,6,0);accept(m,2);
    for(unsigned n=2;n<=65;n++){
        m=base;for(unsigned j=1;j<n;j++)APPEND(m,INS(4,71),5+j,34,1,
            INS(4,71),5+j,33,2+j,INS(4,59),4,5+j,0);
        if(n<=64)accept(m,n);else reject(m);
    }
    m=base;APPEND(m,INS(5,54),9,10,0,11,INS(4,83),4,12,5,INS(1,56));reject(m);
    m=base;APPEND(m,INS(5,54),9,10,0,11,INS(5,65),4,12,5,1,INS(1,56));reject(m);
    m=base;APPEND(m,INS(5,54),9,10,0,11,INS(5,57),9,12,13,5,INS(1,56));reject(m);
    m=base;APPEND(m,INS(5,54),9,10,0,11,INS(4,61),3,12,5,INS(4,83),3,13,12,INS(1,56));accept(m,1);
    for(unsigned access=0;access<=3;access++){
        m=base;memmove(m.words+18,m.words+17,(m.size-17)*4);m.size++;
        m.words[8]=INS(10,25);m.words[17]=access;
        if(access<=2)assert(accept(m,1).access_qualifier==access);else reject(m);
    }
    for(unsigned i=3;i<=7;i++){
        m=base;m.words[8+i]=i==3?7:i==4||i==7?3:2;reject(m);
    }
    m=base;m.words[5]=0;reject(m);
    m=base;m.words[5]=INS(2048,22);reject(m);
    m=base;m.size--;reject(m);
    m=base;m.words[0]=0;reject(m);
    m=base;m.words[3]=1000001;reject(m);
    NvnVkImageType records[64];size_t count=99;const char *error=NULL;
    assert(!nvn_vk_image_types(NULL,0,records,&count,&error)&&!count&&error);checks++;
    printf("image type reflection: %u checks passed\n",checks);
}
int main(int argc,char **argv) {
    if(argc==1){unit_tests();return 0;}
    if(argc!=2)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END)){fclose(f);return 3;}
    long bytes=ftell(f);rewind(f);if(bytes<20||bytes%4){fclose(f);return 3;}
    uint32_t *words=malloc((size_t)bytes);if(!words){fclose(f);return 3;}
    if(fread(words,1,(size_t)bytes,f)!=(size_t)bytes){free(words);fclose(f);return 3;}fclose(f);
    NvnVkImageType records[64];size_t count=0;const char *error=NULL;
    int ok=nvn_vk_image_types(words,(size_t)bytes/4,records,&count,&error);free(words);
    if(!ok){fprintf(stderr,"%s\n",error);return 4;}
    printf("{\"image_count\":%zu,\"images\":[",count);
    for(size_t i=0;i<count;i++){
        NvnVkImageType r=records[i];if(i)putchar(',');
        printf("{\"variable_id\":%u,\"image_type_id\":%u,\"sampled_image_type_id\":%u,"
            "\"set\":%u,\"binding\":%u,\"sampled_type_id\":%u,\"dim\":%u,\"depth\":%u,"
            "\"arrayed\":%u,\"multisampled\":%u,\"sampled\":%u,\"image_format\":%u,\"access_qualifier\":%u}",
            r.variable_id,r.image_type_id,r.sampled_image_type_id,r.descriptor_set,r.binding,
            r.sampled_type_id,r.dim,r.depth,r.arrayed,r.multisampled,r.sampled,r.image_format,r.access_qualifier);
    }
    puts("]}");return 0;
}
