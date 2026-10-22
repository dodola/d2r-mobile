/* 覆盖所有配置的渲染槽和出图槽；由资源锁保护临时资源列表。
 * Account for all configured render slots and the present slot; the resource lock owns temporary lists.
 */
static unsigned memory_contexts(VkSubmission *contexts[5]) {
    unsigned count=0;for(int i=0;i<G.render_slots_n;i++)contexts[count++]=render_slot_ptr(i);
    contexts[count++]=&G.submission_slot2;return count;
}
static void memory_snapshot_ownership(DmVkMemoryStats *out) {
    out->temp_retired_bytes=G.retired_bytes;out->temp_recording_bytes=out->temp_inflight_bytes=0;
    VkSubmission *contexts[5];unsigned count=memory_contexts(contexts);
    for(unsigned k=0;k<count;k++){
        VkSubmission *sub=contexts[k];uint64_t bytes=0;
        for(int i=0;i<sub->ntemporary;i++)bytes+=sub->temporaries[i]->allocation_size;
        if(sub->phase==SUBMISSION_RECORDING)out->temp_recording_bytes+=bytes;
        else out->temp_inflight_bytes+=bytes;
    }
}
int dm_vk_memory_get_stats(DmVkMemoryStats *out) {
    if(!out)return EINVAL;
    if(pthread_mutex_trylock(&G.lock))return EBUSY;
    if(pthread_mutex_trylock(&memory_mu)){pthread_mutex_unlock(&G.lock);return EBUSY;}
    *out=memory_stats;pthread_mutex_unlock(&memory_mu);memory_snapshot_ownership(out);
    pthread_mutex_unlock(&G.lock);return 0;
}
int dm_vk_memory_trim_caches(unsigned flags,DmVkMemoryStats *out) {
    if(flags&~DM_VK_TRIM_ALL)return EINVAL;
    if(pthread_mutex_trylock(&G.lock))return EBUSY;
    VkSubmission *contexts[5];unsigned count=memory_contexts(contexts);
    if(G.active||g_jcount||(G.slot_publication&&nvn_vk_publication_pending(&g_publication))||ghost.n||ghost.busy){pthread_mutex_unlock(&G.lock);return EBUSY;}
    if(G.slot_publication)publication_retire_ready_locked();
    for(unsigned k=0;k<count;k++)if(contexts[k]->phase!=SUBMISSION_UNUSED&&contexts[k]->phase!=SUBMISSION_RETIRED){
        pthread_mutex_unlock(&G.lock);return EBUSY;
    }
    uint64_t started=dm_android_nvn_clock_ns(),freed=0;
    /* The caller blocks new guest graphics while this lock is released again.
     * No submission is recorded, in flight or awaiting CPU publication. */
    if(flags&DM_VK_TRIM_RETIRED)while(G.nretired){VBuffer *b=retired_take(G.nretired-1);freed+=b->allocation_size;buffer_free(b);free(b);}
    if(flags&DM_VK_TRIM_DERIVED)for(unsigned k=0;k<count;k++)resource_cache_clear(contexts[k]);
    if(flags&DM_VK_TRIM_DERIVED)for(uint32_t i=0;i<G.texture_used;i++)if(G.textures[i].serial)freed+=texture_derived_release(&G.textures[i]);
    if(flags&DM_VK_TRIM_WRITEBACK)for(unsigned k=0;k<count;k++){
        if(contexts[k]->wb_stage.m)freed+=contexts[k]->wb_stage.allocation_size;
        buffer_free(&contexts[k]->wb_stage);contexts[k]->wb_staged=0;contexts[k]->wb_stage_total=0;
    }
    if(flags){
        /* Primary image identities and guest registrations survive. Cached
         * descriptors may still name a derived image; rebuild all such plans
         * on the next recording epoch. Executable command buffers are reset
         * by sub_begin_ctx before reuse, after their fences were retired. */
        G.registry_revision++;memset(G.driver_cache,0,sizeof G.driver_cache);
        for(unsigned i=0;i<6;i++){
            G.binding_revision[i]++;G.ssbo_revision[i]++;G.texbind_revision[i]++;
            G.driver_revision[i][0]++;G.driver_revision[i][1]++;
        }
        for(unsigned k=0;k<count;k++)nvn_vk_command_state_reset(&contexts[k]->command_state);
    }
    pthread_mutex_lock(&memory_mu);
    if(flags){
        memory_stats.trim_count++;memory_stats.last_trim_freed_bytes=freed;
        memory_stats.last_trim_ns=dm_android_nvn_clock_ns()-started;
    }
    if(out)*out=memory_stats;
    pthread_mutex_unlock(&memory_mu);
    if(out)memory_snapshot_ownership(out);
    pthread_mutex_unlock(&G.lock);return 0;
}
