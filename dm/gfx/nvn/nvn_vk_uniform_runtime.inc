/* 常量缓冲快照直接读取受脏页跟踪的客体内容，并覆盖同提交的 GPU 写入。
 * Uniform snapshots read dirty-tracked guest contents directly and overlay writes from the same submission.
 */
static int uniform_snapshot(Range r,VkDescriptorBufferInfo *info,uint64_t *serial) {
    if(!G.uniform_versions||!r.addr||!r.size||r.size>65536)return 0;
    uint64_t off=0;if(ghost_pool_override(r.addr,r.size,&off))return 0;
    VPool *p=pool_find(r.addr,r.size,&off);if(!p)die("uniform snapshot pool range",r.addr);
    pool_conflict_wait(p,off,r.size,0);*serial=p->serial;
    /* 乘法取高位可区分连续的 64 KiB 对齐地址，避免低位散列退化。
     * Multiplicative high bits distinguish consecutive 64 KiB aligned addresses instead of aliasing their low bits.
     */
    unsigned bucket=(unsigned)(((r.addr^(r.size*31)^p->serial)*UINT64_C(11400714819323198485))>>52);
    UniformSource *memo=&S.uniform_sources[bucket];int changed=1;
    if(memo->range.addr==r.addr&&memo->range.size==r.size&&memo->serial==p->serial&&memo->epoch==S.epoch){
        changed=nvn_dirty_since((void *)(uintptr_t)r.addr,r.size,memo->stamp);
        for(int i=memo->writes;i<S.nwrites&&!changed;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size))changed=1;
        if(!changed){memo->writes=S.nwrites;*info=memo->info;G.profile_uniform_reuses++;return 1;}
    }
    uint64_t stamp=nvn_dirty_now();nvn_dirty_protect((void *)(uintptr_t)r.addr,r.size);
    uint8_t bytes[65536];memcpy(bytes,(void *)(uintptr_t)r.addr,r.size);int gpu=0;
    for(int i=0;i<S.nwrites;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){
        Range w=S.writes[i];uint64_t lo=r.addr>w.addr?r.addr:w.addr,hi=r.addr+r.size<w.addr+w.size?r.addr+r.size:w.addr+w.size;
        if(S.write_cpu[i])memcpy(bytes+lo-r.addr,S.write_cpu[i]+lo-w.addr,hi-lo);else gpu=1;
    }
    G.profile_uniform_hash_bytes+=r.size;
    uint32_t hash=gpu?0:(uint32_t)crc32(0,bytes,(uInt)r.size),align=(uint32_t)G.properties.limits.minUniformBufferOffsetAlignment;
    if(align<4)align=4;NvnUniformRef ref;
    int stored=nvn_uniform_intern(&S.uniform_arena,bytes,(uint32_t)r.size,align,hash,!gpu,&ref);
    if(!stored&&S.uniform_arena.nblock<NVN_UNIFORM_BLOCKS){
        VBuffer *block=temporary_named(4u<<20,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,TEMP_ALIGNED);
        if(!nvn_uniform_add_block(&S.uniform_arena,block,block->map,(uint32_t)block->size))die("uniform arena block registration",S.epoch);
        stored=nvn_uniform_intern(&S.uniform_arena,bytes,(uint32_t)r.size,align,hash,!gpu,&ref);
    }
    if(!stored){
        /* 容量耗尽时分配独立版本，绝不回退到可变的共享镜像。
         * On capacity exhaustion allocate a private version, never fall back to the mutable shared mirror.
         */
        VBuffer *block=temporary_named((r.size+align-1)/align*align,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,TEMP_ALIGNED);
        memcpy(block->map,bytes,r.size);ref=(NvnUniformRef){block,block->map,0};G.profile_uniform_fallbacks++;
    }
    VBuffer *block=ref.owner;*info=(VkDescriptorBufferInfo){block->b,ref.offset,r.size};
    if(stored==2){G.profile_uniform_reuses++;G.profile_uniform_content_hits++;}else G.profile_uniform_bytes+=r.size;
    if(gpu){
        all_barrier();
        /* 镜像中的写入按命令顺序完成；只拷贝写入范围，未写入部分已经固定。
         * Mirror writes execute in command order; copy only written ranges, while untouched bytes are already frozen.
         */
        for(int i=0;i<S.nwrites;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){
            Range w=S.writes[i];uint64_t lo=r.addr>w.addr?r.addr:w.addr,hi=r.addr+r.size<w.addr+w.size?r.addr+r.size:w.addr+w.size;
            uint64_t src=off+lo-r.addr,dst=ref.offset+lo-r.addr;
            if((src|dst|(hi-lo))&3)die("uniform GPU overlay word alignment",lo);
            VkBufferCopy copy={src,dst,hi-lo};vkCmdCopyBuffer(S.command,p->v.b,block->b,1,&copy);
            pool_use_mark(p,src,hi-lo,0);G.profile_uniform_gpu_bytes+=hi-lo;
        }
        all_barrier();
    }
    *memo=(UniformSource){.range=r,.serial=p->serial,.epoch=S.epoch,.stamp=stamp,.writes=S.nwrites,.info=*info};return 1;
}
