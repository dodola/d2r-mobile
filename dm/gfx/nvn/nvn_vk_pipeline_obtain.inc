/* 需求管线等待及显式降级；从本地 Vulkan 管线模块拆出以直接回归。
 * Demand pipeline waiting and explicit degradation, shared with the direct regression fixture. */
static Pso pipeline_obtain(const PsoKey *key,uint64_t hash,VShader *v,VShader *f,int *skipped) {
    uint64_t t=dm_android_nvn_clock_ns();Pso pso={0};int obtained=0;
    if(skipped)*skipped=0;
    if(PW.started&&PW.mode==2&&!G.disable_pso_reuse&&G.npso<LIMIT_PSOS){
        StablePso stable=pipeline_stable(key);pthread_mutex_lock(&PW.lock);int at=pipeline_find(&stable);
        if(at<0&&PW.count<LIMIT_PSOS){at=(int)PW.count++;PW.jobs[at]=(PipelineJob){.stable=stable,.state=PJ_QUEUED};PW.generation++;}
        if(at>=0){PipelineJob *j=&PW.jobs[at];j->demand=1;pthread_cond_signal(&PW.changed);
            int waited=j->state==PJ_QUEUED||j->state==PJ_BUILDING;if(waited)PW.waits++;
            /* 需求管线必须就绪才画，预热仍并行；冷缓存可停顿但不缺对象。
             * Demand pipelines must be ready before drawing; warm-up stays parallel. Cold caches may stall, but objects stay complete. */
            while(j->state==PJ_QUEUED||j->state==PJ_BUILDING)pthread_cond_wait(&PW.changed,&PW.lock);
            if(j->state==PJ_READY){pso=j->pso;j->state=PJ_CLAIMED;obtained=1;PW.claims++;
                fprintf(stderr,"[nvn-vulkan] PIPELINE_USE seed=%d waited=%d wait_ns=%llu compute=%u\n",j->seed,waited,(unsigned long long)(dm_android_nvn_clock_ns()-t),key->compute);}
        }
        pthread_mutex_unlock(&PW.lock);PW.wait_ns+=dm_android_nvn_clock_ns()-t;
    }
    if(!obtained){uint64_t begin=dm_android_nvn_clock_ns();VkResult result=pipeline_build(key,v,f,&pso);
        if(result!=VK_SUCCESS){
            StablePso stable=pipeline_stable(key);
            if(PG.degrade&&result==VK_ERROR_INVALID_SHADER_NV&&pipe_guard_quarantined(&stable)){
                if(skipped)*skipped=1;
                fprintf(stderr,"[nvn-vulkan] PIPELINE_DEGRADED explicit=1 compute=%u key=%08x\n",key->compute,(uint32_t)crc32(0,(const void*)&stable,sizeof stable));return pso;
            }
            if(pipe_guard_quarantined(&stable))die("pipeline quarantined: gfx-pipe-reset retries; gfx-pipe-degrade=1 explicitly allows missing effects",result);
            CHECK(result);
        }
        PW.sync_creates++;
        fprintf(stderr,"[nvn-vulkan] PIPELINE_CREATE source=synchronous compute=%u result=0 build_ns=%llu\n",key->compute,(unsigned long long)(dm_android_nvn_clock_ns()-begin));
        if(PW.started){StablePso stable=pipeline_stable(key);pthread_mutex_lock(&PW.lock);int at=pipeline_find(&stable);
            if(at<0&&PW.count<LIMIT_PSOS){at=(int)PW.count++;PW.jobs[at]=(PipelineJob){.stable=stable,.state=PJ_CLAIMED};}
            PW.generation++;pthread_cond_signal(&PW.changed);pthread_mutex_unlock(&PW.lock);}
    }
    G.profile_pipeline_ns+=dm_android_nvn_clock_ns()-t;return pso_remember(key,hash,pso);
}
