/* Checked layout shared by Vulkan uploads and offline regression tests.
 * Buffer offsets are bytes; row/image dimensions are always texels, including BC. */
#ifndef NVN_VK_UPLOAD_H
#define NVN_VK_UPLOAD_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint64_t offset,bytes,scratch,stencil;
} NvnVkUpload;

static inline int nvn_vk_upload_mul(uint64_t a,uint64_t b,uint64_t *out) {
    if(b && a>UINT64_MAX/b)return 0;
    *out=a*b;return 1;
}
static inline int nvn_vk_upload_align(uint64_t value,uint64_t alignment,uint64_t *out) {
    if(!alignment)return 0;
    uint64_t remainder=value%alignment,padding=remainder?alignment-remainder:0;
    if(value>UINT64_MAX-padding)return 0;
    *out=value+padding;return 1;
}
/* Append one mip/layer. Split depth/stencil uses a four-byte depth plane and
 * a one-byte stencil plane; every following region starts suitably aligned. */
static inline int nvn_vk_upload_append(uint64_t *total,unsigned w,unsigned h,unsigned d,
        unsigned bw,unsigned bh,unsigned block,unsigned bpp,int native_bc,int split_ds,NvnVkUpload *out) {
    if(!total||!out||!w||!h||!d||!bw||!bh||!block||!bpp||(native_bc&&split_ds))return 0;
    uint64_t columns=((uint64_t)w+bw-1)/bw,rows=((uint64_t)h+bh-1)/bh;
    uint64_t scratch,pixels,bytes,offset,stencil=0;
    if(!nvn_vk_upload_mul(columns,rows,&scratch)||!nvn_vk_upload_mul(scratch,d,&scratch)||
       !nvn_vk_upload_mul(scratch,block,&scratch)||!nvn_vk_upload_mul(w,h,&pixels)||
       !nvn_vk_upload_mul(pixels,d,&pixels))return 0;
    if(native_bc)bytes=scratch;
    else if(!nvn_vk_upload_mul(pixels,split_ds?5:bpp,&bytes))return 0;
    uint64_t unit=split_ds?4:native_bc?block:bpp;
    uint64_t divisor=unit%4==0?4:unit%2==0?2:1;
    uint64_t alignment=unit/divisor*4; /* LCM(texel/block bytes, four bytes). */
    if(!nvn_vk_upload_align(*total,alignment,&offset)||bytes>UINT64_MAX-offset||
       offset+bytes>SIZE_MAX||scratch>SIZE_MAX)return 0;
    if(split_ds)stencil=offset+pixels*4;
    *out=(NvnVkUpload){offset,bytes,scratch,stencil};*total=offset+bytes;return 1;
}
/* Compressed image offsets are block-aligned; an incomplete final block is
 * legal only at the mip edge. Subtraction avoids overflowing x + width. */
static inline int nvn_vk_bc_region(unsigned width,unsigned height,unsigned x,unsigned y,
        unsigned w,unsigned h) {
    return w&&h&&x<=width&&y<=height&&w<=width-x&&h<=height-y&&!(x%4)&&!(y%4)&&
        (!(w%4)||w==width-x)&&(!(h%4)||h==height-y);
}
#endif
