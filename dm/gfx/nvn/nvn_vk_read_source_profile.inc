/* 按资源类别统计声明范围，提交内精确去重，用于决定快照分块和增量策略。
 * Profile declared ranges by resource class with exact per-submission deduplication to size snapshot blocks and incremental copies.
 */
enum {READ_VERTEX,READ_INDEX,READ_UNIFORM,READ_STORAGE,READ_STORAGE_WRITE,READ_COPY,READ_TEXEL,READ_KINDS};
static struct {uint64_t calls,bytes,unique,unique_bytes,max_bytes,overflow;} read_source_stats[READ_KINDS];
static struct {Range range;uint64_t epoch;unsigned kind;} read_source_seen[4096];
static void read_source_note(unsigned kind,Range r) {
    if(!G.pool_range_stats||!r.size||kind>=READ_KINDS)return;
    read_source_stats[kind].calls++;read_source_stats[kind].bytes+=r.size;
    if(r.size>read_source_stats[kind].max_bytes)read_source_stats[kind].max_bytes=r.size;
    uint64_t hash=(r.addr^(r.size*31)^kind)*UINT64_C(11400714819323198485);unsigned slot=(unsigned)(hash>>52);
    for(unsigned n=0;n<4096;n++,slot=(slot+1)&4095){
        if(read_source_seen[slot].epoch!=S.epoch){
            read_source_seen[slot].epoch=S.epoch;read_source_seen[slot].range=r;read_source_seen[slot].kind=kind;
            read_source_stats[kind].unique++;read_source_stats[kind].unique_bytes+=r.size;return;
        }
        if(read_source_seen[slot].kind==kind&&read_source_seen[slot].range.addr==r.addr&&read_source_seen[slot].range.size==r.size)return;
    }
    read_source_stats[kind].overflow++;
}
static void read_source_dump(uint64_t frame) {
    if(!G.pool_range_stats)return;
    static const char *names[]={"vertex","index","uniform","storage_read","storage_write","copy","texel"};
    for(unsigned k=0;k<READ_KINDS;k++)if(read_source_stats[k].calls)
        fprintf(stderr,"[nvn-vulkan] BUFFER_READ_PROFILE frame=%llu kind=%s calls=%llu requested_bytes=%llu unique_ranges=%llu unique_bound_bytes=%llu max_bytes=%llu overflow=%llu\n",
            (unsigned long long)frame,names[k],(unsigned long long)read_source_stats[k].calls,(unsigned long long)read_source_stats[k].bytes,
            (unsigned long long)read_source_stats[k].unique,(unsigned long long)read_source_stats[k].unique_bytes,(unsigned long long)read_source_stats[k].max_bytes,(unsigned long long)read_source_stats[k].overflow);
    memset(read_source_stats,0,sizeof read_source_stats);
}
