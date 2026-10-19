#include "../../gfx/nvn/nvn_vk_shader_index.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void put(uint8_t *p,size_t at,uint32_t value){memcpy(p+at,&value,4);}
int main(int argc,char **argv) {
    uint8_t bytes[2*(24+0x880+64)]={0};size_t stride=sizeof(bytes)/2;
    for(int i=0;i<2;i++) {
        uint8_t *p=bytes+i*stride;put(p,20,64);put(p,24,0x98761234);put(p,28,1);put(p,32,16);put(p,24+0x704,64);
        memset(p+24+0x880,0x40+i,64);
    }
    size_t cursor=0;nvn_shader_index_record a,b;
    assert(nvn_shader_index_read(bytes,sizeof bytes,2,&cursor,&a));
    assert(nvn_shader_index_read(bytes,sizeof bytes,2,&cursor,&b));assert(cursor==sizeof bytes);
    assert(!memcmp(a.control,b.control,0x880));assert(!nvn_shader_index_equal(&a,&b));
    memcpy(bytes+stride+24+0x880,bytes+24+0x880,64);assert(nvn_shader_index_equal(&a,&b));
    cursor=0;assert(!nvn_shader_index_read(bytes,stride-1,2,&cursor,&a));assert(cursor==0);
    put(bytes,20,UINT32_MAX);assert(!nvn_shader_index_read(bytes,sizeof bytes,2,&cursor,&a));
    put(bytes,20,64);put(bytes,24+0x714,1);assert(!nvn_shader_index_read(bytes,sizeof bytes,2,&cursor,&a));
    put(bytes,24+0x714,0);put(bytes,32,17);assert(!nvn_shader_index_read(bytes,sizeof bytes,2,&cursor,&a));
    put(bytes,32,16);assert(!nvn_shader_index_read(bytes,sizeof bytes,3,&cursor,&a));
    cursor=SIZE_MAX;assert(!nvn_shader_index_read(bytes,sizeof bytes,2,&cursor,&a));
    cursor=0;assert(nvn_shader_index_read(bytes,20+0x880,1,&cursor,&a));assert(cursor==20+0x880 && !a.code_size);
    puts("shader index legacy, full-code collision and malformed bounds passed");
    if(argc==2) {
        FILE *file=fopen(argv[1],"rb");assert(file && !fseek(file,0,SEEK_END));long size=ftell(file);
        assert(size>=12 && size<=128*1024*1024 && !fseek(file,0,SEEK_SET));
        uint8_t *data=malloc((size_t)size);assert(data && fread(data,1,(size_t)size,file)==(size_t)size);assert(!fclose(file));
        uint32_t header[3];memcpy(header,data,12);assert(header[0]==0x494b5644 && header[2]<=1000000);cursor=12;
        for(uint32_t i=0;i<header[2];i++)assert(nvn_shader_index_read(data,(size_t)size,header[1],&cursor,&a));
        assert(cursor==(size_t)size);printf("actual cache records=%u passed\n",header[2]);free(data);
    }
}
