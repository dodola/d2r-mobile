/* 录制时固定写回版本，完成线程只发布提交自有数据，资源回收由录制线程执行。
 * Freeze writeback versions during recording; the worker publishes submission-owned data and the recorder retires resources.
 */
static void publication_stage_record(void) {
    S.publication_copy_count=(size_t)S.nwrites;S.publication_raw_bytes=0;S.wb_staged=0;
    for(int i=0;i<S.nwrites;i++){
        Range r=S.writes[i];VPool *p=pool_find(r.addr,r.size,NULL);
        if(!p||!p->v.b)die("publication stage pool lifetime",r.addr);
        S.writeback_plan[i]=(NvnVkWritebackSpan){r.addr,r.size,p->serial};S.publication_raw_bytes+=r.size;
    }
    if(G.enable_writeback_coalesce){
        S.publication_copy_count=nvn_vk_writeback_plan(S.writeback_plan,S.publication_copy_count);
        if(S.publication_copy_count==SIZE_MAX)die("invalid publication plan",S.epoch);
    }
    uint64_t total=0;for(size_t i=0;i<S.publication_copy_count;i++)total+=S.writeback_plan[i].size;
    S.wb_stage_total=total;if(!total)return;
    if(S.wb_stage.size<total){
        buffer_free(&S.wb_stage);uint64_t want=total+(total>>1);if(want<(1ULL<<20))want=1ULL<<20;
        G.want_cached=wb_stage_has_cached_type();
        S.wb_stage=buffer_create_kind(want,VK_BUFFER_USAGE_TRANSFER_DST_BIT,0,DM_VK_MEMORY_WRITEBACK);G.want_cached=0;
    }
    all_barrier();VkBufferCopy copies[64];uint32_t n=0;VkBuffer source=VK_NULL_HANDLE;uint64_t cursor=0;
    for(size_t i=0;i<S.publication_copy_count;i++){
        NvnVkWritebackSpan r=S.writeback_plan[i];uint64_t off=0;VPool *p=pool_find(r.addr,r.size,&off);
        if(!p||p->serial!=r.pool_serial)die("publication stage pool identity",r.addr);
        if(n&&(source!=p->v.b||n==64)){vkCmdCopyBuffer(S.command,source,S.wb_stage.b,n,copies);n=0;}
        source=p->v.b;copies[n++]=(VkBufferCopy){off,cursor,r.size};cursor+=r.size;
    }
    if(n)vkCmdCopyBuffer(S.command,source,S.wb_stage.b,n,copies);S.wb_staged=1;
}

/* 此函数不访问可变池表、资源缓存、当前录制槽或帧统计。
 * This function never accesses mutable pool tables, resource caches, the current recording slot, or frame statistics.
 */
static void publication_effects(VkSubmission *sub,int kind) {
    if(sub->phase!=SUBMISSION_INFLIGHT)die("publication worker phase",sub->epoch);
    CHECK(vkGetFenceStatus(G.device,sub->gpu_fence));sub->phase=SUBMISSION_GPU_DONE;
    if(sub->depth_input_guard_active){
        uint32_t failed=0;memcpy(&failed,sub->depth_input_guard.map,4);
        if(failed)die("depth input projected sample outside calibrated device interval",failed);
    }
    if(kind){if(sub->nwrites||sub->ncounter||sub->nevents)die("present has unpublished guest effects",sub->epoch);return;}
    uint64_t started=dm_android_nvn_thread_cpu_ns(),cursor=0;
    if(sub->wb_staged&&sub->wb_stage.cached)wb_stage_invalidate(sub);
    for(size_t i=0;i<sub->publication_copy_count;i++){
        NvnVkWritebackSpan r=sub->writeback_plan[i];
        if(!sub->wb_staged||r.size>sub->wb_stage_total-cursor)die("publication stage range",r.addr);
        if(G.enable_writeback_prepare)dm_guest_write_prepare((void *)(uintptr_t)r.addr,r.size);
        memcpy((void *)(uintptr_t)r.addr,sub->wb_stage.map+cursor,r.size);cursor+=r.size;
    }
    sub->publication_cpu_ns=dm_android_nvn_thread_cpu_ns()-started;
    uint64_t occ[LIMIT_WRITES],times[LIMIT_WRITES];
    if(sub->nocc)CHECK(vkGetQueryPoolResults(G.device,sub->occlusion,0,sub->nocc,sub->nocc*8,occ,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    if(sub->ntime)CHECK(vkGetQueryPoolResults(G.device,sub->timestamps,0,sub->ntime,sub->ntime*8,times,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT));
    for(int i=0;i<sub->ncounter;i++){
        CounterOp *op=&sub->counter_ops[i];
        if(op->kind==0)G.samples_passed=0;
        else if(op->kind==1)G.samples_passed+=occ[op->query];
        else {
            uint64_t ticks=times[op->query];if(G.timestamp_bits<64)ticks&=(1ULL<<G.timestamp_bits)-1;
            uint64_t stamp=(uint64_t)((long double)ticks*G.properties.limits.timestampPeriod*12/625);
            uint64_t result[2]={op->kind==2?G.samples_passed:stamp,stamp};
            /* 客体写保护跟踪使后续上传失效；不能改写在途 GPU 读取的共享镜像。
             * Guest write protection invalidates later uploads; never mutate a mirror still read by the GPU.
             */
            dm_guest_write_prepare((void *)(uintptr_t)op->address,16);memcpy((void *)(uintptr_t)op->address,result,16);
        }
    }
    for(int i=0;i<sub->nevents;i++){
        Event *e=&sub->events[i];if(e->mode)atomic_fetch_add((_Atomic uint32_t *)e->addr,e->value);
        else atomic_store((_Atomic uint32_t *)e->addr,e->value);
    }
}
static void *publication_worker(void *arg) {
    (void)arg;
    for(;;){
        NvnVkPublicationJob job=nvn_vk_publication_front(&g_publication);VkSubmission *sub=job.context;
        uint64_t started=dm_android_nvn_clock_ns();CHECK(vkWaitForFences(G.device,1,&sub->gpu_fence,VK_TRUE,UINT64_MAX));
        sub->publication_wait_ns=dm_android_nvn_clock_ns()-started;publication_effects(sub,job.kind);
        sub->publication_overlap=atomic_load(&G.recording_active);
        /* 发布后不再访问该槽；录制线程可立即回收，队列完成仅使用任务副本。
         * Never touch the slot after publication: the recorder may retire it immediately; queue completion uses the job copy.
         */
        atomic_store(&G.completed,job.ticket);atomic_store(&sub->phase,SUBMISSION_PUBLISHED);
        if(!nvn_vk_publication_finish(&g_publication,job))die("publication FIFO ownership",job.ticket);
    }
}
static void publication_enqueue(VkSubmission *sub,int kind) {
    static int worker_up;
    if(!worker_up){pthread_t thread;if(pthread_create(&thread,NULL,publication_worker,NULL))die("publication worker creation",0);pthread_detach(thread);worker_up=1;}
    if(!nvn_vk_publication_push(&g_publication,(NvnVkPublicationJob){sub,sub->ticket,kind}))die("publication queue overflow",sub->ticket);
}
/* 调用者持有资源锁；发布者从不取该锁，因此等待发布不会阻塞发布线程。
 * Caller owns the resource lock; the publisher never takes it, so waiting cannot block publication.
 */
static void publication_retire_ready_locked(void) {
    VkSubmission *saved=G.current_submission;int active=G.active,completing=G.completing;
    for(int i=0;i<=G.render_slots_n;i++){
        VkSubmission *sub=i==G.render_slots_n?&G.submission_slot2:render_slot_ptr(i);
        if(atomic_load(&sub->phase)!=SUBMISSION_PUBLISHED)continue;
        G.current_submission=sub;G.completing=1;
        G.profile_completion_wait_ns+=sub->publication_wait_ns;G.profile_completion_waits++;
        G.profile_submission_publications++;G.profile_overlap_publications+=sub->publication_overlap;
        if(sub->slot_index<0)present_after_gpu();
        else{
            CommitInfo ci;memcpy(&ci,&sub->commit_info,sizeof ci);ci.wait_cpu=dm_android_nvn_thread_cpu_ns();commit_after_gpu(ci,0);
        }
    }
    G.current_submission=saved;G.active=active;G.completing=completing;
}
