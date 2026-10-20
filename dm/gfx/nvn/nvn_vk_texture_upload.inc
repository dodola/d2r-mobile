/* Adapted from vickwv/d2r-mobile 68852db / 0f107b3. Unlike that version,
 * compressed strides use texels, and every mip/layer offset is aligned. */
static int texture_bc_format(unsigned format,VkFormat *out,uint8_t *block) {
    switch(format) {
    case 0x41:case 0x42:*out=VK_FORMAT_BC1_RGBA_UNORM_BLOCK;*block=8;return 1;
    case 0x45:case 0x46:*out=VK_FORMAT_BC1_RGBA_SRGB_BLOCK;*block=8;return 1;
    case 0x43:*out=VK_FORMAT_BC2_UNORM_BLOCK;break;
    case 0x47:*out=VK_FORMAT_BC2_SRGB_BLOCK;break;
    case 0x44:*out=VK_FORMAT_BC3_UNORM_BLOCK;break;
    case 0x48:*out=VK_FORMAT_BC3_SRGB_BLOCK;break;
    case 0x49:*out=VK_FORMAT_BC4_UNORM_BLOCK;*block=8;return 1;
    case 0x4a:*out=VK_FORMAT_BC4_SNORM_BLOCK;*block=8;return 1;
    case 0x4b:*out=VK_FORMAT_BC5_UNORM_BLOCK;break;
    case 0x4c:*out=VK_FORMAT_BC5_SNORM_BLOCK;break;
    case 0x4d:*out=VK_FORMAT_BC7_UNORM_BLOCK;break;
    case 0x4e:*out=VK_FORMAT_BC7_SRGB_BLOCK;break;
    case 0x4f:*out=VK_FORMAT_BC6H_SFLOAT_BLOCK;break;
    case 0x50:*out=VK_FORMAT_BC6H_UFLOAT_BLOCK;break;
    default:return 0;
    }
    *block=16;return 1;
}
static int texture_native_bc(const tex_t *source,VkFormat *format,uint8_t *block) {
    if(!g_bc_passthrough || (source->d.flags&NVN_TEXTURE_FLAGS_IMAGE) ||
       !texture_bc_format(source->d.format,format,block))return 0;
    /* Native compressed images are a sampled/transfer representation. Retain
     * decoding for guest storage images and unsupported dimensions/formats. */
    int target=source->d.target;
    if(target!=NVN_TEXTURE_TARGET_2D&&target!=NVN_TEXTURE_TARGET_2D_ARRAY&&
       target!=NVN_TEXTURE_TARGET_CUBEMAP&&target!=NVN_TEXTURE_TARGET_CUBEMAP_ARRAY)return 0;
    VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if((format_properties(*format).optimalTilingFeatures&required)!=required)return 0;
    int cube=target==NVN_TEXTURE_TARGET_CUBEMAP||target==NVN_TEXTURE_TARGET_CUBEMAP_ARRAY;
    VkImageCreateFlags flags=cube||(target==NVN_TEXTURE_TARGET_2D_ARRAY&&source->d.width==source->d.height&&
        source->L.layers>=6&&source->d.width<=G.properties.limits.maxImageDimensionCube)?VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT:0;
    VkImageFormatProperties props;
    VkResult result=vkGetPhysicalDeviceImageFormatProperties(G.physical,*format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,flags,&props);
    return result==VK_SUCCESS&&source->d.width<=props.maxExtent.width&&source->d.height<=props.maxExtent.height&&
        source->d.levels<=props.maxMipLevels&&source->L.layers<=props.maxArrayLayers;
}
static void texture_upload(VTexture *t,const tex_t *source,const uint8_t *memory,int upload_bc) {
    if(source->d.flags&NVN_TEXTURE_FLAGS_LINEAR)die("linear texture upload layout",source->d.flags);
    uint64_t count64=(uint64_t)source->L.layers*source->d.levels;
    if(!count64||count64>UINT32_MAX/2||count64>SIZE_MAX/sizeof(NvnVkUpload))die("texture upload subresource count",count64);
    unsigned count=(unsigned)count64;
    NvnVkUpload *plans=calloc(count,sizeof(*plans));
    VkBufferImageCopy *copies=calloc((size_t)count*2,sizeof(*copies));
    if(!plans||!copies)die("texture upload plan allocation",count);
    VkImageAspectFlags mask=aspect(t->format);int split=(mask&VK_IMAGE_ASPECT_STENCIL_BIT)!=0;
    if(split&&t->format!=VK_FORMAT_D24_UNORM_S8_UINT&&t->format!=VK_FORMAT_D32_SFLOAT_S8_UINT)
        die("packaged depth/stencil upload format",source->d.format);
    unsigned group=g_texture_upload_batch?count:1;
    for(unsigned first=0;first<count;first+=group){
        unsigned end=first+group;uint64_t total=0,scratch_max=0;
        for(unsigned i=first;i<end;i++){
            unsigned level=i%source->d.levels;
            unsigned w=t->width>>level,h=t->height>>level,d=t->depth>>level;if(!w)w=1;if(!h)h=1;if(!d)d=1;
            int bw,bh,block=nvn_fmt_bpb(source->d.format,&bw,&bh);
            if(block<=0||!nvn_vk_upload_append(&total,w,h,d,bw,bh,block,t->bpp,t->bc_block!=0,split,&plans[i]))
                die("texture upload layout/overflow",source->serial);
            if(plans[i].scratch>scratch_max)scratch_max=plans[i].scratch;
        }
        VBuffer *staging=temporary(total,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        uint8_t *scratch=malloc((size_t)scratch_max);if(!scratch)die("texture upload scratch allocation",scratch_max);
        unsigned n=0;
        for(unsigned i=first;i<end;i++){
            unsigned level=i%source->d.levels,layer=i/source->d.levels;
            unsigned w=t->width>>level,h=t->height>>level,d=t->depth>>level;if(!w)w=1;if(!h)h=1;if(!d)d=1;
            int bw,bh,block=nvn_fmt_bpb(source->d.format,&bw,&bh);
            uint64_t row=((uint64_t)w+bw-1)/bw*block,rows=((uint64_t)h+bh-1)/bh;
            if(nvn_deswizzle(&source->d,level,layer,memory,scratch,row))die("Tegra texture deswizzle",level);
            NvnVkUpload plan=plans[i];uint8_t *dst=staging->map+plan.offset;
            VkBufferImageCopy copy={.bufferOffset=plan.offset,.imageSubresource={split?VK_IMAGE_ASPECT_DEPTH_BIT:mask,level,layer,1},.imageExtent={w,h,d}};
            if(split){
                if(block!=(t->format==VK_FORMAT_D24_UNORM_S8_UINT?4:8))die("depth/stencil upload block size",block);
                uint64_t pixels=(uint64_t)w*h*d;
                for(uint64_t pixel=0;pixel<pixels;pixel++){
                    uint32_t depth;memcpy(&depth,scratch+pixel*block,4);
                    staging->map[plan.stencil+pixel]=block==4?(uint8_t)(depth>>24):scratch[pixel*block+4];
                    if(block==4)depth&=0xffffff;
                    memcpy(dst+pixel*4,&depth,4);
                }
                copies[n++]=copy;copy.bufferOffset=plan.stencil;copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_STENCIL_BIT;
            }else if(t->bc_block)memcpy(dst,scratch,(size_t)plan.bytes);
            else if(upload_bc){
                for(unsigned z=0;z<d;z++)if(nvn_bc_decode(source->d.format,scratch+z*row*rows,row,w,h,
                    dst+(size_t)z*w*h*t->bpp,(size_t)w*t->bpp))die("BC decode",source->d.format);
            }else memcpy(dst,scratch,(size_t)plan.bytes);
            copies[n++]=copy;
        }
        free(scratch);vkCmdCopyBufferToImage(S.command,staging->b,t->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,n,copies);
    }
    free(copies);free(plans);
}
