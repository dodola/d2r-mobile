#include "../../gfx/nvn/nvn_vk_distance_capabilities.h"
#include <assert.h>
#include <stdio.h>

static void unit_tests(void) {
    const uint32_t member[]={0x07230203,0x10300,0,20,0,
        2u<<16|17,1,5u<<16|72,5,0,11,3,5u<<16|72,5,1,11,4,
        4u<<16|30,5,6,6};
    size_t count=0;uint32_t mask=0;const char *error=NULL;
    uint32_t *out=nvn_vk_distance_capabilities(member,sizeof member/4,&count,&mask,&error);
    assert(out&&!error&&mask==3&&count==sizeof member/4+4);
    assert(!memcmp(out,member,20)&&!memcmp(out+9,member+5,sizeof member-20));
    size_t again_count=0;uint32_t again_mask=0;
    uint32_t *again=nvn_vk_distance_capabilities(out,count,&again_count,&again_mask,&error);
    assert(again&&again_count==count&&again_mask==3&&!memcmp(out,again,count*4));free(again);free(out);
    const uint32_t variable[]={0x07230203,0x10300,0,20,0,2u<<16|17,32,4u<<16|71,6,11,3};
    out=nvn_vk_distance_capabilities(variable,sizeof variable/4,&count,&mask,&error);
    assert(out&&mask==1&&count==sizeof variable/4&&!memcmp(variable,out,sizeof variable));free(out);
    const uint32_t none[]={0x07230203,0x10300,0,20,0,2u<<16|17,1};
    out=nvn_vk_distance_capabilities(none,sizeof none/4,&count,&mask,&error);
    assert(out&&!mask&&count==sizeof none/4&&!memcmp(none,out,sizeof none));free(out);
    uint32_t broken[sizeof member/4];
    memcpy(broken,member,sizeof member);broken[9]=2; /* invalid member index */
    assert(!nvn_vk_distance_capabilities(broken,sizeof broken/4,&count,&mask,&error)&&!count&&!mask&&error);
    memcpy(broken,member,sizeof member);broken[8]=20; /* struct ID beyond bound */
    assert(!nvn_vk_distance_capabilities(broken,sizeof broken/4,&count,&mask,&error));
    memcpy(broken,member,sizeof member);broken[5]=0;
    assert(!nvn_vk_distance_capabilities(broken,sizeof broken/4,&count,&mask,&error));
    assert(!nvn_vk_distance_capabilities(member,sizeof member/4-1,&count,&mask,&error));
    assert(!nvn_vk_distance_capabilities(NULL,0,&count,&mask,&error));
    puts("distance capabilities: member/variable declarations, preservation, idempotence and bounds passed");
}

int main(int argc,char **argv) {
    if(argc==1){unit_tests();return 0;}
    if(argc!=3)return 2;
    FILE *f=fopen(argv[1],"rb");if(!f)return 3;
    if(fseek(f,0,SEEK_END)){fclose(f);return 3;}
    long bytes=ftell(f);rewind(f);
    if(bytes<20||bytes%4){fclose(f);return 3;}
    uint32_t *input=malloc((size_t)bytes);
    if(!input){fclose(f);return 3;}
    if(fread(input,1,(size_t)bytes,f)!=(size_t)bytes){free(input);fclose(f);return 3;}
    fclose(f);size_t count=0;uint32_t mask=0;const char *error=NULL;
    uint32_t *output=nvn_vk_distance_capabilities(input,(size_t)bytes/4,&count,&mask,&error);free(input);
    if(!output){fprintf(stderr,"%s\n",error);return 4;}
    f=fopen(argv[2],"wb");if(!f){free(output);return 3;}
    int written=fwrite(output,4,count,f)==count;int closed=fclose(f)==0;free(output);
    if(!written||!closed)return 3;
    printf("required_mask=%u words=%zu\n",mask,count);return 0;
}
