/* Execute the production upload encoder with a CPU copy recorder. This does
 * not emulate or validate a Vulkan driver; it verifies its input bytes/regions. */
#include <vulkan/vulkan.h>
#include "../nvn/nvn_obj.h"
#include "../nvn/nvn_vk_upload.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_bcdec.h"
#include "../../host/dm_yield_backoff.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct { uint8_t *map;VkBuffer b;uint64_t size; } VBuffer;
typedef struct { uint32_t width,height,depth,bpp;VkFormat format;VkImage image;uint8_t bc_block;
    VkImageLayout layout;uint8_t *cpu_snapshot;uint64_t serial,upload_stamp; } VTexture;
static struct { VkPhysicalDevice physical;VkPhysicalDeviceProperties properties;uint64_t uploads,profile_decode_ns; } G;
static struct { VkCommandBuffer command; } S;
static int g_bc_passthrough,g_texture_upload_batch;
static VBuffer buffer;
static unsigned checks,calls,regions_seen;
static tex_t expected_source;
static VTexture expected_texture;
static uint8_t *expected[4096];
static int refresh_mode;
static uint8_t refreshed[512];
static VkFormatFeatureFlags supported=~0u;
static VkResult query_result=VK_SUCCESS;
static void check(int value){checks++;assert(value);}
static void die(const char *where,long long value){fprintf(stderr,"%s: %lld\n",where,value);abort();}
static VkFormatProperties format_properties(VkFormat format){(void)format;return (VkFormatProperties){.optimalTilingFeatures=supported};}
static VkImageAspectFlags aspect(VkFormat format){
    return format==VK_FORMAT_D24_UNORM_S8_UINT||format==VK_FORMAT_D32_SFLOAT_S8_UINT?
        VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT:VK_IMAGE_ASPECT_COLOR_BIT;
}
static VBuffer *temporary(uint64_t size,VkBufferUsageFlags usage){
    (void)usage;free(buffer.map);buffer.map=calloc(1,(size_t)size);check(buffer.map!=NULL);buffer.size=size;return &buffer;
}
static VkResult mock_query(VkPhysicalDevice device,VkFormat format,VkImageType type,VkImageTiling tiling,
        VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties *props){
    (void)device;(void)format;(void)type;(void)tiling;(void)usage;(void)flags;
    *props=(VkImageFormatProperties){.maxExtent={4096,4096,1},.maxMipLevels=16,.maxArrayLayers=512};return query_result;
}
#define vkGetPhysicalDeviceImageFormatProperties mock_query
static void mock_copy(VkCommandBuffer command,VkBuffer src,VkImage dst,VkImageLayout layout,unsigned count,const VkBufferImageCopy *copies){
    (void)command;(void)src;(void)dst;check(layout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);calls++;
    for(unsigned i=0;i<count;i++){
        VkBufferImageCopy c=copies[i];unsigned level=c.imageSubresource.mipLevel,layer=c.imageSubresource.baseArrayLayer;
        unsigned w=expected_texture.width>>level,h=expected_texture.height>>level,d=expected_texture.depth>>level;
        if(!w)w=1;if(!h)h=1;if(!d)d=1;
        unsigned index=layer*expected_source.d.levels+level;
        if(refresh_mode){
            check(c.imageSubresource.aspectMask==VK_IMAGE_ASPECT_COLOR_BIT&&c.imageSubresource.layerCount==1);
            check(c.imageExtent.depth==1&&c.imageOffset.z==0);
            int bw,bh,block=nvn_fmt_bpb(expected_source.d.format,&bw,&bh);
            unsigned x=(unsigned)c.imageOffset.x,y=(unsigned)c.imageOffset.y,rw=c.imageExtent.width,rh=c.imageExtent.height;
            if(expected_texture.bc_block){
                check(nvn_vk_bc_region(w,h,x,y,rw,rh));
                check(c.bufferRowLength==((w+3)/4)*4&&c.bufferImageHeight==((h+3)/4)*4);
                size_t pitch=(size_t)c.bufferRowLength/4*block,tight=(size_t)((rw+3)/4)*block;
                for(unsigned row=0;row<(rh+3)/4;row++){
                    check(c.bufferOffset+row*pitch+tight<=buffer.size);
                    memcpy(refreshed+((size_t)(y/4+row)*((w+3)/4)+x/4)*block,buffer.map+c.bufferOffset+row*pitch,tight);
                }
            }else{
                check(c.bufferRowLength==w&&c.bufferImageHeight==h);
                size_t pitch=(size_t)w*expected_texture.bpp,tight=(size_t)rw*expected_texture.bpp;
                for(unsigned row=0;row<rh;row++){
                    check(c.bufferOffset+row*pitch+tight<=buffer.size);
                    memcpy(refreshed+((size_t)(y+row)*w+x)*expected_texture.bpp,buffer.map+c.bufferOffset+row*pitch,tight);
                }
            }
            regions_seen++;continue;
        }
        check(index<4096&&expected[index]!=NULL);check(c.imageExtent.width==w&&c.imageExtent.height==h&&c.imageExtent.depth==d);
        check(c.imageOffset.x==0&&c.imageOffset.y==0&&c.imageOffset.z==0&&c.imageSubresource.layerCount==1);
        check(c.bufferRowLength==0&&c.bufferImageHeight==0);
        int bw,bh;int block=nvn_fmt_bpb(expected_source.d.format,&bw,&bh);
        uint64_t pixels=(uint64_t)w*h*d,bytes=pixels*expected_texture.bpp;
        uint8_t *want=expected[index],*decoded=NULL;
        if(expected_texture.bc_block){
            bytes=((uint64_t)w+3)/4*((h+3)/4)*d*block;
            check(c.bufferOffset%(unsigned)block==0);
        }else if(c.imageSubresource.aspectMask!=VK_IMAGE_ASPECT_COLOR_BIT){
            bytes=pixels*(c.imageSubresource.aspectMask==VK_IMAGE_ASPECT_DEPTH_BIT?4:1);
            decoded=malloc((size_t)bytes);check(decoded!=NULL);
            for(uint64_t n=0;n<pixels;n++){
                uint32_t depth;memcpy(&depth,want+n*block,4);
                if(c.imageSubresource.aspectMask==VK_IMAGE_ASPECT_STENCIL_BIT)decoded[n]=block==4?(uint8_t)(depth>>24):want[n*block+4];
                else {if(block==4)depth&=0xffffff;memcpy(decoded+n*4,&depth,4);}
            }
            want=decoded;
        }else if(bw==4){
            uint64_t row=((uint64_t)w+3)/4*block,rows=(h+3)/4;
            decoded=malloc((size_t)bytes);check(decoded!=NULL);
            for(unsigned z=0;z<d;z++)check(!nvn_bc_decode(expected_source.d.format,want+z*row*rows,row,w,h,
                decoded+(size_t)z*w*h*expected_texture.bpp,(size_t)w*expected_texture.bpp));
            want=decoded;
        }
        check(c.bufferOffset%4==0&&c.bufferOffset<=buffer.size&&bytes<=buffer.size-c.bufferOffset);
        check(!memcmp(buffer.map+c.bufferOffset,want,(size_t)bytes));free(decoded);regions_seen++;
    }
}
#define vkCmdCopyBufferToImage mock_copy
#include "../nvn/nvn_vk_texture_upload.inc"
static uint64_t nvn_dirty_now(void){return 1;}
static void nvn_dirty_protect(const void *memory,size_t size){(void)memory;(void)size;}
static uint64_t dm_android_nvn_clock_ns(void){return 0;}
static void all_barrier(void){}
static void texture_transition(VTexture *t,VkImageLayout layout){t->layout=layout;}
static void texture_snapshot_set(VTexture *t,uint8_t *data,uint64_t size){(void)size;free(t->cpu_snapshot);t->cpu_snapshot=data;}
static void tex_dirty(VTexture *t){(void)t;}
#include "../nvn/nvn_vk_texture_refresh.inc"

static void run_upload(unsigned format,unsigned bpp,unsigned w,unsigned h,unsigned depth,unsigned layers,unsigned levels,int native){
    tex_t source={.d={.target=depth>1?NVN_TEXTURE_TARGET_3D:layers>1?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D,
        .format=(int)format,.width=w,.height=h,.depth=depth>1?depth:layers,.levels=levels},.serial=1};
    nvn_tex_layout(&source.d,&source.L);source.mem=calloc(1,(size_t)source.L.size);check(source.mem!=NULL);
    VTexture texture={.width=w,.height=h,.depth=depth,.bpp=bpp};uint8_t block=0;
    int bc=texture_bc_format(format,&texture.format,&block);
    if(native)texture.bc_block=block;
    else texture.format=format==NVN_FORMAT_DEPTH24_STENCIL8?VK_FORMAT_D24_UNORM_S8_UINT:
        format==NVN_FORMAT_DEPTH32F_STENCIL8?VK_FORMAT_D32_SFLOAT_S8_UINT:VK_FORMAT_R8G8B8A8_UNORM;
    for(unsigned layer=0;layer<source.L.layers;layer++)for(unsigned level=0;level<levels;level++){
        unsigned x=w>>level,y=h>>level,z=depth>>level;if(!x)x=1;if(!y)y=1;if(!z)z=1;
        int bw,bh,bb=nvn_fmt_bpb(format,&bw,&bh);size_t row=((size_t)x+bw-1)/bw*bb,bytes=row*((y+bh-1)/bh)*z;
        unsigned index=layer*levels+level;check(index<4096);expected[index]=malloc(bytes);check(expected[index]!=NULL);
        for(size_t i=0;i<bytes;i++)expected[index][i]=(uint8_t)(i*31+layer*17+level*3);
        check(!nvn_swizzle(&source.d,level,layer,expected[index],row,source.mem));
    }
    expected_source=source;expected_texture=texture;
    for(int batch=0;batch<=1;batch++){
        g_texture_upload_batch=batch;calls=regions_seen=0;texture_upload(&texture,&source,source.mem,bc);
        check(calls==(batch?1:source.L.layers*levels));
        check(regions_seen==source.L.layers*levels*((aspect(texture.format)&VK_IMAGE_ASPECT_STENCIL_BIT)?2u:1u));
    }
    for(unsigned i=0;i<source.L.layers*levels;i++){free(expected[i]);expected[i]=NULL;}
    free(source.mem);
}
static void refresh_checks(int native){
    tex_t source={.d={.target=NVN_TEXTURE_TARGET_2D,.format=0x44,.width=9,.height=5,.depth=1,.levels=1},.serial=1};
    nvn_tex_layout(&source.d,&source.L);source.mem=calloc(1,(size_t)source.L.size);check(source.mem!=NULL);
    uint8_t baseline[96],next[96],want[512];
    for(unsigned i=0;i<sizeof baseline;i++)baseline[i]=(uint8_t)(i*13);
    memcpy(next,baseline,sizeof next);memset(next+16,0xc1,16);memset(next+80,0xd2,16);
    check(!nvn_swizzle(&source.d,0,0,baseline,48,source.mem));
    VTexture t={.width=9,.height=5,.depth=1,.bpp=4,.bc_block=native?16:0,.serial=1};
    t.cpu_snapshot=malloc((size_t)source.L.size);check(t.cpu_snapshot!=NULL);memcpy(t.cpu_snapshot,source.mem,(size_t)source.L.size);
    if(native){memcpy(refreshed,baseline,sizeof baseline);memcpy(want,next,sizeof next);}
    else {
        check(!nvn_bc_decode(0x44,baseline,48,9,5,refreshed,36));
        check(!nvn_bc_decode(0x44,next,48,9,5,want,36));
    }
    /* The GPU changed the first block while RAM on the same tracked page did
     * not. Refreshing two other blocks must retain every GPU-owned byte. */
    if(native){memset(refreshed,0x7a,16);memset(want,0x7a,16);}
    else for(unsigned y=0;y<4;y++){memset(refreshed+y*36,0x7a,16);memset(want+y*36,0x7a,16);}
    check(!nvn_swizzle(&source.d,0,0,next,48,source.mem));
    expected_source=source;expected_texture=t;refresh_mode=1;calls=regions_seen=0;
    refresh_compressed_cpu(&t,&source);check(calls==1&&regions_seen==2);
    check(!memcmp(refreshed,want,native?sizeof next:9*5*4));
    calls=regions_seen=0;refresh_compressed_cpu(&t,&source);check(calls==0&&regions_seen==0);
    refresh_mode=0;free(t.cpu_snapshot);free(source.mem);
}
static void layout_checks(void){
    NvnVkUpload plan;uint64_t total=1;
    check(nvn_vk_upload_append(&total,1,1,1,1,1,1,1,0,0,&plan)&&plan.offset==4&&total==5);
    check(nvn_vk_upload_append(&total,1,1,1,4,4,16,4,1,0,&plan)&&plan.offset==16&&total==32);
    total=UINT64_MAX-2;uint64_t before=total;
    check(!nvn_vk_upload_append(&total,1,1,1,1,1,4,4,0,0,&plan)&&total==before);
    total=0;check(!nvn_vk_upload_append(&total,UINT32_MAX,UINT32_MAX,UINT32_MAX,1,1,16,16,0,0,&plan));
    total=1;check(nvn_vk_upload_append(&total,1,1,1,1,1,12,12,0,0,&plan)&&plan.offset==12&&total==24);
    total=3;check(nvn_vk_upload_append(&total,1,1,1,1,1,3,3,0,0,&plan)&&plan.offset==12&&total==15);
    check(!nvn_vk_upload_append(&total,0,1,1,1,1,1,1,0,0,&plan));
    check(nvn_vk_bc_region(9,5,4,4,5,1));check(!nvn_vk_bc_region(9,5,1,0,4,4));
    check(!nvn_vk_bc_region(9,5,0,0,5,4));check(!nvn_vk_bc_region(9,5,8,4,2,1));
    check(!nvn_vk_bc_region(9,5,0,0,0,4));
    DmYieldBackoff state={0};for(int i=0;i<16;i++)check(!dm_yield_backoff_due(&state,0));
    check(dm_yield_backoff_due(&state,15999999));check(!dm_yield_backoff_due(&state,16000000));
    DmYieldBackoff other={0};check(!dm_yield_backoff_due(&other,16000000));
}
static void capability_checks(void){
    tex_t source={.d={.target=NVN_TEXTURE_TARGET_2D,.format=0x44,.width=64,.height=32,.levels=6},.L={.layers=1}};
    VkFormat format;uint8_t block;G.properties.limits.maxImageDimensionCube=4096;
    check(!texture_native_bc(&source,&format,&block));g_bc_passthrough=1;
    check(texture_native_bc(&source,&format,&block)&&format==VK_FORMAT_BC3_UNORM_BLOCK&&block==16);
    source.d.flags=NVN_TEXTURE_FLAGS_IMAGE;check(!texture_native_bc(&source,&format,&block));source.d.flags=0;
    supported=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;check(!texture_native_bc(&source,&format,&block));supported=~0u;
    query_result=VK_ERROR_FORMAT_NOT_SUPPORTED;check(!texture_native_bc(&source,&format,&block));query_result=VK_SUCCESS;
    source.d.width=8192;check(!texture_native_bc(&source,&format,&block));
}
int main(void){
    layout_checks();capability_checks();
    run_upload(NVN_FORMAT_R8,1,3,5,1,2,3,0);
    run_upload(NVN_FORMAT_RG8,2,7,3,1,3,3,0);
    run_upload(NVN_FORMAT_RGBA8,4,8,4,4,1,4,0);
    run_upload(NVN_FORMAT_DEPTH24_STENCIL8,4,3,5,1,2,3,0);
    run_upload(NVN_FORMAT_DEPTH32F_STENCIL8,8,7,3,1,1,3,0);
    run_upload(0x49,1,9,5,1,2,4,0);run_upload(0x49,1,9,5,1,2,4,1);
    run_upload(0x44,4,12,8,1,3,4,0);run_upload(0x44,4,12,8,1,3,4,1);
    run_upload(0x49,1,64,64,1,252,7,0);run_upload(0x49,1,64,64,1,252,7,1);
    refresh_checks(0);refresh_checks(1);
    free(buffer.map);printf("TEXTURE_UPLOAD checks=%u failures=0 gpu_executed=0\n",checks);return 0;
}
