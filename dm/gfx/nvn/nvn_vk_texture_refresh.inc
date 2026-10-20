/* GPU transfers and CPU streaming can update different compression blocks of
 * one NVN allocation. Compare actual block bytes to the last CPU snapshot so
 * page-level dirtiness never overwrites unrelated GPU-owned contents. */
static void refresh_compressed_cpu(VTexture *t,const tex_t *source) {
    uint64_t started=dm_android_nvn_clock_ns(),stamp=nvn_dirty_now();nvn_dirty_protect(source->mem,source->L.size);
    uint8_t *snapshot=malloc(source->L.size);if(!snapshot)die("streamed BC snapshot allocation",source->L.size);
    memcpy(snapshot,source->mem,source->L.size);
    unsigned changed_regions=0;
    for(unsigned layer=0;layer<source->L.layers;layer++)for(unsigned level=0;level<source->d.levels;level++){
        unsigned width=t->width>>level,height=t->height>>level,depth=t->depth>>level;if(!width)width=1;if(!height)height=1;if(!depth)depth=1;
        int bw,bh;int block=nvn_fmt_bpb(source->d.format,&bw,&bh);unsigned columns=(width+bw-1)/bw,rows=(height+bh-1)/bh;
        uint64_t row=(uint64_t)columns*block,linear_bytes=row*rows*depth;
        uint8_t *old=malloc(linear_bytes),*next=malloc(linear_bytes);if(!old||!next)die("streamed BC block comparison allocation",linear_bytes);
        if(nvn_deswizzle(&source->d,level,layer,t->cpu_snapshot,old,row)||nvn_deswizzle(&source->d,level,layer,snapshot,next,row))die("streamed BC block layout",source->serial);
        if(!memcmp(old,next,linear_bytes)){free(old);free(next);continue;}
        uint64_t plane=t->bc_block?row*rows:((uint64_t)width*height*t->bpp+3)&~3ULL;
        VBuffer *staging=temporary(plane*depth,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if(t->bc_block)memcpy(staging->map,next,(size_t)linear_bytes);
        else for(unsigned z=0;z<depth;z++)if(nvn_bc_decode(source->d.format,next+z*row*rows,row,width,height,staging->map+z*plane,(size_t)width*t->bpp))die("streamed BC block decode",source->d.format);
        VkBufferImageCopy *regions=NULL;unsigned count=0,capacity=0;
        for(unsigned z=0;z<depth;z++)for(unsigned y=0;y<rows;y++){
            unsigned x=0;while(x<columns){uint64_t offset=((uint64_t)z*rows+y)*row+(uint64_t)x*block;
                if(!memcmp(old+offset,next+offset,block)){x++;continue;}
                unsigned first=x++;while(x<columns&&memcmp(old+((uint64_t)z*rows+y)*row+(uint64_t)x*block,next+((uint64_t)z*rows+y)*row+(uint64_t)x*block,block))x++;
                if(count==capacity){capacity=capacity?capacity*2:64;VkBufferImageCopy *larger=realloc(regions,(size_t)capacity*sizeof(*regions));if(!larger)die("streamed BC copy regions",capacity);regions=larger;}
                unsigned px=first*bw,py=y*bh,w=(x-first)*bw,h=bh;if(w>width-px)w=width-px;if(h>height-py)h=height-py;
                regions[count++]=(VkBufferImageCopy){.bufferOffset=t->bc_block?offset:z*plane+((uint64_t)py*width+px)*t->bpp,
                    .bufferRowLength=t->bc_block?columns*(unsigned)bw:width,.bufferImageHeight=t->bc_block?rows*(unsigned)bh:height,
                    .imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,layer,1},.imageOffset={px,py,z},.imageExtent={w,h,1}};
            }
        }
        all_barrier();texture_transition(t,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(S.command,staging->b,t->image,t->layout,count,regions);changed_regions+=count;
        free(regions);free(old);free(next);
    }
    texture_snapshot_set(t,snapshot,source->L.size);t->upload_stamp=stamp;
    if(changed_regions){tex_dirty(t);G.uploads++;fprintf(stderr,"[nvn-vulkan] BC_STREAM_REFRESH serial=%llu changed_regions=%u gpu_blocks_preserved=1\n",(unsigned long long)t->serial,changed_regions);}
    G.profile_decode_ns+=dm_android_nvn_clock_ns()-started;
}
