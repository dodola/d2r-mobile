/* Vertex fetch policy is separate from image-format policy. No replacement
 * fetch may read beyond the original NVN attribute. */
#ifndef NVN_VK_VERTEX_H
#define NVN_VK_VERTEX_H
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkFormat native,head,tail;
    uint32_t bytes,tail_offset,convert,kind,alignment; /* kind: float=0, uint=1, sint=2 */
} NvnVkVertexFormat;

static NvnVkVertexFormat nvn_vk_vertex_format(unsigned f) {
    static const VkFormat formats[4][12]={
        {VK_FORMAT_R8_UNORM,VK_FORMAT_R8_SNORM,VK_FORMAT_R8_UINT,VK_FORMAT_R8_SINT,VK_FORMAT_R16_SFLOAT,VK_FORMAT_R16_UNORM,VK_FORMAT_R16_SNORM,VK_FORMAT_R16_UINT,VK_FORMAT_R16_SINT,VK_FORMAT_R32_SFLOAT,VK_FORMAT_R32_UINT,VK_FORMAT_R32_SINT},
        {VK_FORMAT_R8G8_UNORM,VK_FORMAT_R8G8_SNORM,VK_FORMAT_R8G8_UINT,VK_FORMAT_R8G8_SINT,VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_R16G16_UNORM,VK_FORMAT_R16G16_SNORM,VK_FORMAT_R16G16_UINT,VK_FORMAT_R16G16_SINT,VK_FORMAT_R32G32_SFLOAT,VK_FORMAT_R32G32_UINT,VK_FORMAT_R32G32_SINT},
        {VK_FORMAT_R8G8B8_UNORM,VK_FORMAT_R8G8B8_SNORM,VK_FORMAT_R8G8B8_UINT,VK_FORMAT_R8G8B8_SINT,VK_FORMAT_R16G16B16_SFLOAT,VK_FORMAT_R16G16B16_UNORM,VK_FORMAT_R16G16B16_SNORM,VK_FORMAT_R16G16B16_UINT,VK_FORMAT_R16G16B16_SINT,VK_FORMAT_R32G32B32_SFLOAT,VK_FORMAT_R32G32B32_UINT,VK_FORMAT_R32G32B32_SINT},
        {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_R8G8B8A8_SNORM,VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_R8G8B8A8_SINT,VK_FORMAT_R16G16B16A16_SFLOAT,VK_FORMAT_R16G16B16A16_UNORM,VK_FORMAT_R16G16B16A16_SNORM,VK_FORMAT_R16G16B16A16_UINT,VK_FORMAT_R16G16B16A16_SINT,VK_FORMAT_R32G32B32A32_SFLOAT,VK_FORMAT_R32G32B32A32_UINT,VK_FORMAT_R32G32B32A32_SINT}};
    static const unsigned widths[12]={1,1,1,1,2,2,2,2,2,4,4,4};
    NvnVkVertexFormat p={0};
    unsigned group=0,kind=0;
    if(f>=1&&f<=0x30){group=(f-1)/12;kind=(f-1)%12;}
    else if(f>=0x51&&f<=0x68){
        unsigned k=f-0x51;group=k/6;unsigned width=k%6/2;
        p.convert=(k&1)?2:1;kind=(unsigned[]){2,7,10}[width]+(k&1);
    }else {
        switch(f){
        case 0x3d:p.native=VK_FORMAT_A2B10G10R10_UNORM_PACK32;break;
        case 0x3e:p.native=VK_FORMAT_A2B10G10R10_UINT_PACK32;break;
        case 0x3f:p.native=VK_FORMAT_B10G11R11_UFLOAT_PACK32;break;
        case 0x69:p.native=VK_FORMAT_A2B10G10R10_SNORM_PACK32;break;
        case 0x6a:p.native=VK_FORMAT_A2B10G10R10_SINT_PACK32;break;
        case 0x6b:p.native=VK_FORMAT_A2B10G10R10_UINT_PACK32;p.convert=1;break;
        case 0x6c:p.native=VK_FORMAT_A2B10G10R10_SINT_PACK32;p.convert=2;break;
        default:return p;
        }
        p.kind=(f==0x3e)?1:(f==0x6a)?2:0;
        p.head=p.native;p.bytes=p.alignment=4;return p;
    }
    p.native=p.head=formats[group][kind];p.bytes=(group+1)*widths[kind];
    p.alignment=widths[kind];
    p.kind=p.convert?0:(kind==2||kind==7||kind==10)?1:(kind==3||kind==8||kind==11)?2:0;
    if(group==2){p.head=formats[1][kind];p.tail=formats[0][kind];p.tail_offset=2*widths[kind];}
    return p;
}

static VkFormat nvn_vk_vertex_scaled_native(unsigned f,unsigned components) {
    if(f<0x51||f>0x68||!components||components>4)return VK_FORMAT_UNDEFINED;
    unsigned k=f-0x51,width=k%6/2;
    if(width==2)return VK_FORMAT_UNDEFINED;
    const unsigned offset[4]={0,7,14,28};
    return (VkFormat)((width?72:11)+(k&1)+(width?7*(components-1):offset[components-1]));
}

/* Unsigned/signed scaled fetch is expressed as exact integer fetch followed by
 * OpConvertUToF/OpConvertSToF. Vulkan has no 32-bit scaled vertex formats. */
static int nvn_vk_vertex_extent(uint64_t size,uint32_t offset,uint32_t bytes,
                                uint32_t stride,uint64_t last) {
    return offset<=size && bytes<=size-offset &&
        (!stride || last<=(size-offset-bytes)/stride);
}
#endif
