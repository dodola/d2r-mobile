/* 本地 R8/RG8 解码的尺寸、边缘块与写入边界回归，覆盖 BC4/BC5 有/无符号。
 * Layout, edge-block and write-boundary regression for local R8/RG8 decoding, covering signed/unsigned BC4/BC5. */
#include "../tex/nvn_bcdec.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
    unsigned cases=0;
    for(uint32_t format=0x49;format<=0x4c;format++)for(unsigned w=1;w<=9;w++)for(unsigned h=1;h<=9;h++){
        uint32_t pixel;int bpp;assert(nvn_bc_unpacked(format,&pixel,&bpp));assert(bpp==(format>=0x4b?2:1));
        size_t pitch=((w+3)/4)*(format>=0x4b?16:8),in=pitch*((h+3)/4),out=w*h*bpp;
        uint8_t *src=calloc(1,in),*dst=malloc(out+32);assert(src&&dst);memset(dst,0xcd,out+32);
        assert(!nvn_bc_decode(format,src,pitch,w,h,dst,w*bpp));
        for(size_t k=out;k<out+32;k++)assert(dst[k]==0xcd);
        free(src);free(dst);cases++;
    }
    printf("BC8_LAYOUT PASS cases=%u R8_RG8 signed_unsigned edge_blocks bounds\n",cases);
}
