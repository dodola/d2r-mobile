/* 非常量读取的提交版本：小范围复用追加 arena，大范围按槽保留并增量更新。
 * Submission versions for non-uniform reads: small ranges use an append-only arena; large ranges persist per slot with incremental updates.
 */
#include "nvn_vk_snapshot_patch.h"
#define READ_SNAPSHOT_USAGE (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT)
/* 三组轮转的约 20 MB 只读表必须同时驻留，否则每帧都会逐出下一组。
 * Keep all three rotating roughly 20 MB read-only tables resident instead of evicting the next table every frame.
 */
#define READ_CACHE_BYTES (96u<<20)
/* 同一 draw/dispatch 的可写别名必须保持共享存储，不能冻结成输入版本。
 * Writable aliases within one draw/dispatch must keep shared storage, not become frozen input versions.
 */
static void read_snapshot_writers(VShader *a,VShader *b) {
    S.read_writer_count=0;VShader *shaders[]={a,b};
    for(unsigned stage=0;stage<2;stage++)if(shaders[stage]){
        VShader *s=shaders[stage];
        for(unsigned i=0;i<s->h.nb;i++){
            RBinding *binding=&s->b[i];Range range={0};int slot=binding->slot;
            if((binding->kind==3||binding->kind==9)&&binding->flags&&slot>=0&&slot<16)
                range=binding->kind==9?G.state.uniform[s->h.stage][slot]:G.state.storage[s->h.stage][slot];
            else if(binding->type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER&&slot>=0&&slot<128){
                unsigned id=G.state.img[s->h.stage][slot]&0xfffff;
                if(id<G.texture_slots&&G.tslot[id].live){tex_t *t=&G.tslot[id].texture;range=(Range){(uint64_t)(uintptr_t)t->mem,t->L.size};}
            }
            if(range.addr&&range.size)S.read_writer_ranges[S.read_writer_count++]=range;
        }
    }
}
static int read_snapshot_aliases_writer(Range r) {
    for(unsigned i=0;i<S.read_writer_count;i++)if(overlap(r.addr,r.size,S.read_writer_ranges[i].addr,S.read_writer_ranges[i].size))return 1;
    return 0;
}
static uint64_t read_snapshot_cache_clear(VkSubmission *sub,uint64_t serial) {
    uint64_t freed=0;
    for(unsigned i=0;i<READ_CACHE_ENTRIES;i++){
        ReadPersistent *e=&sub->read_cache[i];if(!e->buffer.b||(serial&&e->serial!=serial))continue;
        freed+=e->buffer.allocation_size;sub->read_cache_bytes-=e->buffer.allocation_size;
        buffer_free(&e->buffer);memset(e,0,sizeof *e);
    }
    return freed;
}
static void read_snapshot_forget_pool(uint64_t serial) {
    for(int i=0;i<G.render_slots_n;i++)read_snapshot_cache_clear(render_slot_ptr(i),serial);
    read_snapshot_cache_clear(&G.submission_slot2,serial);
}
static ReadPersistent *read_cache_find(Range r,uint64_t serial) {
    for(unsigned i=0;i<READ_CACHE_ENTRIES;i++){
        ReadPersistent *e=&S.read_cache[i];
        if(e->buffer.b&&e->serial==serial&&e->range.addr==r.addr&&e->range.size==r.size)return e;
    }
    return NULL;
}
/* 当前提交已引用的缓存不可覆盖或逐出；容量不足时返回独立临时版本。
 * A cache referenced by this submission cannot be overwritten or evicted; exhaustion falls back to a private temporary version.
 */
static ReadPersistent *read_cache_create(Range r,uint64_t serial) {
    uint64_t size=(r.size+65535)&~UINT64_C(65535);if(size<r.size||size>READ_CACHE_BYTES)return NULL;
    for(;;){
        ReadPersistent *free_entry=NULL,*oldest=NULL;
        for(unsigned i=0;i<READ_CACHE_ENTRIES;i++){
            ReadPersistent *e=&S.read_cache[i];if(!e->buffer.b){if(!free_entry)free_entry=e;continue;}
            if(e->epoch!=S.epoch&&(!oldest||e->epoch<oldest->epoch))oldest=e;
        }
        if(free_entry&&S.read_cache_bytes<=READ_CACHE_BYTES-size){
            free_entry->buffer=buffer_create_kind(size,READ_SNAPSHOT_USAGE,0,DM_VK_MEMORY_TEMP);
            S.read_cache_bytes+=free_entry->buffer.allocation_size;free_entry->range=r;free_entry->serial=serial;return free_entry;
        }
        if(!oldest)return NULL;
        S.read_cache_bytes-=oldest->buffer.allocation_size;buffer_free(&oldest->buffer);memset(oldest,0,sizeof *oldest);
    }
}
static int read_source_current(const UniformSource *memo,Range r,uint64_t serial) {
    if(memo->epoch!=S.epoch||memo->serial!=serial||memo->range.addr!=r.addr||memo->range.size!=r.size)return 0;
    if(nvn_dirty_since((void *)(uintptr_t)r.addr,r.size,memo->stamp))return 0;
    for(int i=memo->writes;i<S.nwrites;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size))return 0;
    return 1;
}
static int read_snapshot(Range r,unsigned kind,VkDescriptorBufferInfo *info) {
    if(!G.buffer_versions||!r.addr||!r.size)return 0;
    if(kind!=READ_COPY&&read_snapshot_aliases_writer(r))return 0;
    uint64_t off=0;VPool *p=ghost_pool_override(r.addr,r.size,&off);
    if(p){*info=(VkDescriptorBufferInfo){p->v.b,off,r.size};return 1;}
    p=pool_find(r.addr,r.size,&off);if(!p)die("read snapshot pool range",r.addr);
    pool_conflict_wait(p,off,r.size,0);
    unsigned bucket=(unsigned)(((r.addr^(r.size*31)^p->serial)*UINT64_C(11400714819323198485))>>52);
    UniformSource *memo=&S.read_sources[bucket];ReadPersistent *cache=NULL;
    if(read_source_current(memo,r,p->serial)){memo->writes=S.nwrites;*info=memo->info;G.profile_read_reuses[kind]++;return 1;}
    if(r.size>65536){
        cache=read_cache_find(r,p->serial);
        if(cache&&read_source_current(&cache->current,r,p->serial)){
            cache->current.writes=S.nwrites;*memo=cache->current;*info=memo->info;G.profile_read_reuses[kind]++;return 1;
        }
        if(!cache)cache=read_cache_create(r,p->serial);
    }
    int gpu=0,overlaid=0;
    for(int i=0;i<S.nwrites;i++)if(overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){
        overlaid=1;if(!S.write_cpu[i])gpu=1;
    }
    uint64_t stamp=nvn_dirty_now();nvn_dirty_protect((void *)(uintptr_t)r.addr,r.size);
    uint32_t align=(uint32_t)G.properties.limits.minUniformBufferOffsetAlignment;
    if(align<G.properties.limits.minStorageBufferOffsetAlignment)align=(uint32_t)G.properties.limits.minStorageBufferOffsetAlignment;
    if(align<G.properties.limits.minTexelBufferOffsetAlignment)align=(uint32_t)G.properties.limits.minTexelBufferOffsetAlignment;
    if(align<4)align=4;
    VBuffer *buffer=NULL;uint64_t offset=0;uint8_t *bytes=NULL,scratch[65536];int persistent=cache&&cache->epoch!=S.epoch;
    if(r.size<=sizeof scratch){memcpy(scratch,(void *)(uintptr_t)r.addr,r.size);bytes=scratch;}
    else{
        buffer=persistent?&cache->buffer:temporary_named((r.size+align-1)/align*align,READ_SNAPSHOT_USAGE,TEMP_ALIGNED);
        uint64_t copied=gpu&&fully_gpu_owned(r)?0:nvn_vk_snapshot_patch(buffer->map,(void *)(uintptr_t)r.addr,r.size,persistent?cache->stamp:0);
        if(G.pool_range_stats&&r.size>1000000&&G.present_count>=700&&G.present_count<704)
            fprintf(stderr,"[nvn-vulkan] READ_PATCH_DETAIL frame=%llu kind=%u addr=%llx size=%llu copied=%llu persistent=%d stamp=%llu gpu=%d overlaid=%d cache_bytes=%llu\n",(unsigned long long)G.present_count,kind,(unsigned long long)r.addr,(unsigned long long)r.size,(unsigned long long)copied,persistent,(unsigned long long)(persistent?cache->stamp:0),gpu,overlaid,(unsigned long long)S.read_cache_bytes);
        if(copied==UINT64_MAX)die("invalid read snapshot dirty span",r.addr);
        G.profile_read_bytes[kind]+=copied;if(persistent)G.profile_read_incremental_bytes+=copied;bytes=buffer->map;
    }
    for(int i=0;i<S.nwrites;i++)if(S.write_cpu[i]&&overlap(r.addr,r.size,S.writes[i].addr,S.writes[i].size)){
        Range w=S.writes[i];uint64_t lo=r.addr>w.addr?r.addr:w.addr,hi=r.addr+r.size<w.addr+w.size?r.addr+r.size:w.addr+w.size;
        memcpy(bytes+lo-r.addr,S.write_cpu[i]+lo-w.addr,hi-lo);
    }
    if(r.size<=sizeof scratch){
        /* 来源版本已去重；避免为内容去重读取慢速的 GPU 映射内存。
         * Source versions are already deduplicated; avoid slow reads of GPU mapped memory for content deduplication.
         */
        NvnUniformRef ref;
        int stored=nvn_uniform_intern(&S.read_arena,bytes,(uint32_t)r.size,align,0,0,&ref);
        if(!stored&&S.read_arena.nblock<NVN_UNIFORM_BLOCKS){
            VBuffer *block=temporary_named(4u<<20,READ_SNAPSHOT_USAGE,TEMP_ALIGNED);
            if(!nvn_uniform_add_block(&S.read_arena,block,block->map,(uint32_t)block->size))die("read arena block registration",S.epoch);
            stored=nvn_uniform_intern(&S.read_arena,bytes,(uint32_t)r.size,align,0,0,&ref);
        }
        if(!stored){VBuffer *block=temporary_named((r.size+align-1)/align*align,READ_SNAPSHOT_USAGE,TEMP_ALIGNED);
            memcpy(block->map,bytes,r.size);ref=(NvnUniformRef){block,block->map,0};G.profile_read_fallbacks++;}
        buffer=ref.owner;offset=ref.offset;if(stored==2)G.profile_read_reuses[kind]++;else G.profile_read_bytes[kind]+=r.size;
    }
    *info=(VkDescriptorBufferInfo){buffer->b,offset,r.size};
    if(gpu){
        all_barrier();
        uint64_t cursor=r.addr;Range span;
        while(nvn_vk_snapshot_next_write(r,S.writes,S.nwrites,&cursor,&span)){
            uint64_t lo=span.addr,hi=lo+span.size;
            uint64_t src=off+lo-r.addr,dst=offset+lo-r.addr;
            if((src|dst|(hi-lo))&3)die("read snapshot GPU overlay word alignment",lo);
            VkBufferCopy copy={src,dst,hi-lo};vkCmdCopyBuffer(S.command,p->v.b,buffer->b,1,&copy);
            pool_use_mark(p,src,hi-lo,0);G.profile_read_gpu_bytes[kind]+=hi-lo;
        }
        all_barrier();
    }
    *memo=(UniformSource){.range=r,.serial=p->serial,.epoch=S.epoch,.stamp=stamp,.writes=S.nwrites,.info=*info};
    if(cache){cache->current=*memo;if(persistent)cache->stamp=overlaid?0:stamp;cache->epoch=S.epoch;}
    return 1;
}
static VkDescriptorBufferInfo buffer_read_range(Range r,unsigned kind) {
    if(G.buffer_versions){
        Range source=r;uint64_t prefix=0;
        /* 顶点和 16 位索引允许子字边界，快照包含池内填充以保持 GPU 拷贝字对齐。
         * Vertices and 16-bit indices may have sub-word edges; include in-pool padding to keep GPU copies word-aligned.
         */
        if(kind==READ_VERTEX||kind==READ_INDEX){
            prefix=r.addr&3;uint64_t size=(r.size+prefix+3)&~UINT64_C(3);
            if(size>=r.size&&pool_find(r.addr-prefix,size,NULL))source=(Range){r.addr-prefix,size};else prefix=0;
        }
        VkDescriptorBufferInfo info;if(read_snapshot(source,kind,&info)){info.offset+=prefix;info.range=r.size;return info;}
    }
    uint64_t off=0;VPool *p=pool_read(r.addr,r.size,&off);return (VkDescriptorBufferInfo){p->v.b,off,r.size};
}
static void read_snapshot_profile_dump(uint64_t frame) {
    if(!G.buffer_versions)return;
    static const char *names[]={"vertex","index","uniform","storage_read","storage_write","copy","texel"};
    for(unsigned k=0;k<READ_KINDS;k++)if(G.profile_read_bytes[k]||G.profile_read_gpu_bytes[k]||G.profile_read_reuses[k])
        fprintf(stderr,"[nvn-vulkan] READ_SNAPSHOT_PROFILE frame=%llu kind=%s copied_bytes=%llu gpu_overlay_bytes=%llu reuses=%llu\n",
            (unsigned long long)frame,names[k],(unsigned long long)G.profile_read_bytes[k],(unsigned long long)G.profile_read_gpu_bytes[k],(unsigned long long)G.profile_read_reuses[k]);
    uint64_t cache_bytes=0;for(int i=0;i<G.render_slots_n;i++)cache_bytes+=render_slot_ptr(i)->read_cache_bytes;
    fprintf(stderr,"[nvn-vulkan] READ_SNAPSHOT_CACHE frame=%llu allocated_bytes=%llu incremental_bytes=%llu private_fallbacks=%llu\n",
        (unsigned long long)frame,(unsigned long long)cache_bytes,(unsigned long long)G.profile_read_incremental_bytes,(unsigned long long)G.profile_read_fallbacks);
    memset(G.profile_read_bytes,0,sizeof G.profile_read_bytes);memset(G.profile_read_gpu_bytes,0,sizeof G.profile_read_gpu_bytes);memset(G.profile_read_reuses,0,sizeof G.profile_read_reuses);
    G.profile_read_incremental_bytes=G.profile_read_fallbacks=0;
}
