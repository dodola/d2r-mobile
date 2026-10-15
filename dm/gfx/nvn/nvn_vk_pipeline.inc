/* Vulkan counterpart of upstream warm_start / pso_user_record / warm_claim.
 * Record only complete pipelines actually requested; never a shader cross-product.
 * Stable shader names replace process-local shader indices in the disk manifest.
 * One compiler owns pending work. Replay may wait for a required pipeline, but
 * never skips a draw or uses a substitute pipeline. No worker takes G.lock. */
typedef struct {PsoKey key;char shader[2][17];} StablePso;
enum { PJ_QUEUED=1,PJ_BUILDING,PJ_READY,PJ_FAILED,PJ_CLAIMED,PJ_DORMANT };
typedef struct {StablePso stable;Pso pso;int state,demand,seed;VkResult result;const VShader *demand_shader[2];} PipelineJob;
static struct {
    pthread_mutex_t lock,cache_lock;pthread_cond_t changed;pthread_t thread;
    PipelineJob jobs[LIMIT_PSOS];unsigned count;int mode,started,trace;
    char cache_path[1200],manifest_path[1200];DmPipelineIdentity identity;
    uint64_t generation,saved_generation,last_save,loaded_bytes;
    uint64_t sync_creates,claims,waits,wait_ns;
    _Atomic uint64_t background_creates,background_ns;
    _Atomic unsigned warm_pending;
} PW={.lock=PTHREAD_MUTEX_INITIALIZER,.cache_lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
static VkSpecializationInfo pipeline_specialization(const VShader *s,const uint32_t *values,VkSpecializationMapEntry *entries) {
    uint32_t n=0;for(unsigned i=0;i<s->h.nb;i++)if(s->storage_bias.mask&(1ull<<i))entries[n++]=(VkSpecializationMapEntry){s->storage_bias.spec_id[i],i*4,4};
    return (VkSpecializationInfo){n,entries,LIMIT_BINDINGS*4,values};
}
/* This is the single factory for demand compilation and manifest warm-up.
 * Its only inputs are immutable key bytes, shader objects and device handles. */
static VkResult pipeline_build(const PsoKey *k,const VShader *v,const VShader *f,Pso *out) {
    memset(out,0,sizeof *out);VkDescriptorSetLayout layouts[]={v->layout,f?f->layout:VK_NULL_HANDLE};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=f?2u:1u,.pSetLayouts=layouts};
    VkResult result=vkCreatePipelineLayout(G.device,&li,NULL,&out->layout);if(result!=VK_SUCCESS)return result;
    VkSpecializationMapEntry maps[2][LIMIT_BINDINGS];VkSpecializationInfo spec[]={pipeline_specialization(v,k->storage_bias[0],maps[0]),{0}};
    if(f)spec[1]=pipeline_specialization(f,k->storage_bias[1],maps[1]);
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=k->compute?VK_SHADER_STAGE_COMPUTE_BIT:VK_SHADER_STAGE_VERTEX_BIT,.module=v->module,.pName="main",.pSpecializationInfo=spec[0].mapEntryCount?&spec[0]:NULL},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=f?f->module:VK_NULL_HANDLE,.pName="main",.pSpecializationInfo=spec[1].mapEntryCount?&spec[1]:NULL}};
    VkRenderPass pass=VK_NULL_HANDLE;
    if(k->compute){
        VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage=stages[0],.layout=out->layout};
        pthread_mutex_lock(&PW.cache_lock);result=vkCreateComputePipelines(G.device,G.pipeline_cache,1,&ci,NULL,&out->pipeline);pthread_mutex_unlock(&PW.cache_lock);
    }else{
        const PassKey *p=&k->pass;
        VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=p->color_count,.pColorAttachments=p->color,.pDepthStencilAttachment=p->has_depth?&p->depth:NULL};
        VkRenderPassCreateInfo ri={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=p->count,.pAttachments=p->attachments,.subpassCount=1,.pSubpasses=&sub,.dependencyCount=p->dependency_count,.pDependencies=p->dependencies};
        result=vkCreateRenderPass(G.device,&ri,NULL,&pass);if(result!=VK_SUCCESS)goto done;
        VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=k->binding_count,.pVertexBindingDescriptions=k->bindings,.vertexAttributeDescriptionCount=k->attribute_count,.pVertexAttributeDescriptions=k->attributes};
        VkPipelineInputAssemblyStateCreateInfo ai={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=k->topology};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
        VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.cullMode=k->cull,.frontFace=k->front,.lineWidth=1};
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo ds={.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=k->depth_test,.depthWriteEnable=k->depth_write,.depthCompareOp=k->depth_compare,.stencilTestEnable=k->stencil_test,.front=k->stencil[0],.back=k->stencil[1]};
        VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=k->color_count,.pAttachments=k->blend};
        VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=3,.pDynamicStates=dynamic};
        VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.flags=k->flags,.stageCount=f?2u:1u,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ai,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=out->layout,.renderPass=pass};
        pthread_mutex_lock(&PW.cache_lock);result=vkCreateGraphicsPipelines(G.device,G.pipeline_cache,1,&gp,NULL,&out->pipeline);pthread_mutex_unlock(&PW.cache_lock);
    }
done:
    if(pass)vkDestroyRenderPass(G.device,pass,NULL);
    if(result!=VK_SUCCESS){if(out->pipeline)vkDestroyPipeline(G.device,out->pipeline,NULL);vkDestroyPipelineLayout(G.device,out->layout,NULL);memset(out,0,sizeof *out);}
    return result;
}
static int pipeline_stable_valid(const StablePso *s) {
    const PsoKey *k=&s->key;
    for(unsigned i=0;i<2;i++)if(s->shader[i][16]|| (i==0||s->shader[i][0]?strspn(s->shader[i],"0123456789abcdef")!=16:0))return 0;
    if(k->compute>1||k->shader[0]||k->shader[1]||k->attribute_count>32||k->binding_count>16||k->color_count>8)return 0;
    if(k->compute)return !s->shader[1][0];
    const PassKey *p=&k->pass;if(!p->count||p->count>9||p->color_count!=k->color_count||p->color_count>8||p->has_depth>1||p->dependency_count>3)return 0;
    for(unsigned i=0;i<p->color_count;i++)if(p->color[i].attachment!=VK_ATTACHMENT_UNUSED&&p->color[i].attachment>=p->count)return 0;
    if(p->has_depth&&p->depth.attachment>=p->count)return 0;
    return 1;
}
static StablePso pipeline_stable(const PsoKey *key) {
    StablePso s={0};s.key=*key;
    for(unsigned i=0;i<2;i++){if(key->shader[i])memcpy(s.shader[i],G.shaders[key->shader[i]-1].key,17);s.key.shader[i]=0;}
    return s;
}
static int pipeline_find(const StablePso *s) {for(unsigned i=0;i<PW.count;i++)if(!memcmp(&PW.jobs[i].stable,s,sizeof *s))return (int)i;return -1;}
static void pipeline_manifest_load(const char *path,int warm) {
    size_t size=0;StablePso *keys=dm_pipeline_read(path,2,&PW.identity,LIMIT_PSOS*sizeof(*keys),&size);
    if(!keys)return;
    if(size%sizeof(*keys)){free(keys);return;}
    for(size_t i=0;i<size/sizeof(*keys)&&PW.count<LIMIT_PSOS;i++)if(pipeline_stable_valid(&keys[i])&&pipeline_find(&keys[i])<0){
        PipelineJob *j=&PW.jobs[PW.count++];j->stable=keys[i];j->state=warm?PJ_QUEUED:PJ_DORMANT;j->seed=warm;
        if(warm)atomic_fetch_add(&PW.warm_pending,1);
    }
    free(keys);
}
static void pipeline_cache_init(void) {
    const char *dir=getenv("DM_VK_PIPELINE_DIR"),*mode=getenv("DM_VK_PIPELINE_MODE");
    PW.mode=dir&&*dir?(mode?atoi(mode):2):0;if(PW.mode<0||PW.mode>2)PW.mode=2;
    PW.trace=getenv("DM_VK_PIPELINE_TRACE")!=NULL;
    PW.identity=(DmPipelineIdentity){.vendor=G.properties.vendorID,.device=G.properties.deviceID,.driver=G.properties.driverVersion,.key_size=sizeof(StablePso),.options=(!getenv("DM_DISABLE_STORAGE_BIAS"))|((unsigned)G.feedback_layout<<1)};
    memcpy(PW.identity.uuid,G.properties.pipelineCacheUUID,16);
    size_t size=0;void *bytes=NULL;
    if(PW.mode){
        size_t index_size=0;uint8_t *index=read_file("shader_index.bin",&index_size);PW.identity.shader_crc=crc32(0,index,(uInt)index_size);free(index);
        if(snprintf(PW.cache_path,sizeof PW.cache_path,"%s/pipeline-cache.bin",dir)>=(int)sizeof PW.cache_path||snprintf(PW.manifest_path,sizeof PW.manifest_path,"%s/pso_user.vkbin",dir)>=(int)sizeof PW.manifest_path)die("pipeline cache path",0);
        bytes=dm_pipeline_read(PW.cache_path,1,&PW.identity,64u*1024*1024,&size);
        if(bytes){VkPipelineCacheHeaderVersionOne h;
            if(size<sizeof h){free(bytes);bytes=NULL;size=0;}else{memcpy(&h,bytes,sizeof h);
                if(h.headerSize!=sizeof h||h.headerVersion!=VK_PIPELINE_CACHE_HEADER_VERSION_ONE||h.vendorID!=G.properties.vendorID||h.deviceID!=G.properties.deviceID||memcmp(h.pipelineCacheUUID,G.properties.pipelineCacheUUID,16)){free(bytes);bytes=NULL;size=0;}}}
    }
    if(!getenv("DM_DISABLE_PIPELINE_CACHE")){
        VkPipelineCacheCreateInfo ci={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,.initialDataSize=size,.pInitialData=bytes};
        VkResult result=vkCreatePipelineCache(G.device,&ci,NULL,&G.pipeline_cache);
        if(result!=VK_SUCCESS&&bytes){ci.initialDataSize=0;ci.pInitialData=NULL;size=0;result=vkCreatePipelineCache(G.device,&ci,NULL,&G.pipeline_cache);}
        CHECK(result);PW.loaded_bytes=size;
    }
    free(bytes);fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE mode=%d loaded_bytes=%llu shader_crc=%08x key_size=%zu\n",PW.mode,(unsigned long long)PW.loaded_bytes,PW.identity.shader_crc,sizeof(StablePso));
}
typedef struct WarmShader {struct WarmShader *next;VShader shader;} WarmShader;
static WarmShader *pipeline_shaders;
static VShader *pipeline_shader(const char *key,unsigned stage) {
    if(!*key)return NULL;
    for(WarmShader *p=pipeline_shaders;p;p=p->next)if(!strcmp(p->shader.key,key)&&p->shader.h.stage==stage)return &p->shader;
    char path[1200];snprintf(path,sizeof path,"%s/%s.vkmeta",G.shader_root,key);FILE *f=fopen(path,"rb");if(!f)return NULL;
    RHeader h;int ok=fread(&h,1,sizeof h,f)==sizeof h&&h.magic==0x4d4b5644&&h.version==1&&h.stage==stage;fclose(f);if(!ok)return NULL;
    snprintf(path,sizeof path,"%s/%s.spv%s",G.shader_root,key,h.reserved?".gz":"");if(access(path,R_OK))return NULL;
    WarmShader *p=calloc(1,sizeof *p);if(!p)return NULL;shader_load_into(&p->shader,key,stage,1);p->next=pipeline_shaders;pipeline_shaders=p;return &p->shader;
}
static void pipeline_checkpoint(void) {
    pthread_mutex_lock(&PW.lock);uint64_t generation=PW.generation;unsigned count=PW.count;
    StablePso *keys=malloc(count*sizeof(*keys));if(keys)for(unsigned i=0;i<count;i++)keys[i]=PW.jobs[i].stable;
    pthread_mutex_unlock(&PW.lock);
    int manifest_ok=!count||(keys&&dm_pipeline_write(PW.manifest_path,2,&PW.identity,keys,count*sizeof(*keys)));free(keys);
    size_t size=0;void *bytes=NULL;VkResult result=VK_SUCCESS;
    if(G.pipeline_cache){pthread_mutex_lock(&PW.cache_lock);
        result=vkGetPipelineCacheData(G.device,G.pipeline_cache,&size,NULL);
        if(result==VK_SUCCESS&&size&&size<=64u*1024*1024){bytes=malloc(size);if(bytes)result=vkGetPipelineCacheData(G.device,G.pipeline_cache,&size,bytes);}
        pthread_mutex_unlock(&PW.cache_lock);}
    int cache_ok=!G.pipeline_cache||(bytes&&result==VK_SUCCESS&&dm_pipeline_write(PW.cache_path,1,&PW.identity,bytes,size));free(bytes);
    pthread_mutex_lock(&PW.lock);PW.last_save=dm_android_nvn_clock_ns();if(manifest_ok&&cache_ok)PW.saved_generation=generation;pthread_mutex_unlock(&PW.lock);
    fprintf(stderr,"[nvn-vulkan] PIPELINE_STORE entries=%u cache_bytes=%zu manifest_ok=%d cache_ok=%d generation=%llu\n",count,size,manifest_ok,cache_ok,(unsigned long long)generation);
}
static void *pipeline_worker(void *unused) {
    (void)unused;
    for(;;){
        pthread_mutex_lock(&PW.lock);int at=-1;
        for(unsigned i=0;i<PW.count;i++)if(PW.jobs[i].state==PJ_QUEUED&&(at<0||PW.jobs[i].demand)){at=(int)i;if(PW.jobs[i].demand)break;}
        uint64_t now=dm_android_nvn_clock_ns();int save=PW.generation!=PW.saved_generation&&now-PW.last_save>=5000000000ull;
        const VShader *demand_v=NULL,*demand_f=NULL;
        if(at>=0){PW.jobs[at].state=PJ_BUILDING;demand_v=PW.jobs[at].demand_shader[0];demand_f=PW.jobs[at].demand_shader[1];}
        if(at<0&&!save){struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec++;pthread_cond_timedwait(&PW.changed,&PW.lock,&deadline);pthread_mutex_unlock(&PW.lock);continue;}
        pthread_mutex_unlock(&PW.lock);
        if(at>=0){PipelineJob *j=&PW.jobs[at];uint64_t t=dm_android_nvn_clock_ns();
            const VShader *v=demand_v?demand_v:pipeline_shader(j->stable.shader[0],j->stable.key.compute?5u:0u);
            const VShader *f=demand_v?demand_f:pipeline_shader(j->stable.shader[1],1);
            Pso pso={0};VkResult result=v&&(!j->stable.shader[1][0]||f)?pipeline_build(&j->stable.key,v,f,&pso):VK_ERROR_INITIALIZATION_FAILED;
            uint64_t elapsed=dm_android_nvn_clock_ns()-t;
            atomic_fetch_add(&PW.background_ns,elapsed);if(result==VK_SUCCESS)atomic_fetch_add(&PW.background_creates,1);
            pthread_mutex_lock(&PW.lock);j->pso=pso;j->result=result;j->state=result==VK_SUCCESS?PJ_READY:PJ_FAILED;PW.generation++;if(j->seed)atomic_fetch_sub(&PW.warm_pending,1);pthread_cond_broadcast(&PW.changed);pthread_mutex_unlock(&PW.lock);
            fprintf(stderr,"[nvn-vulkan] PIPELINE_CREATE source=%s compute=%u result=%d build_ns=%llu key=%08x\n",j->seed?"manifest":"demand-worker",j->stable.key.compute,result,(unsigned long long)elapsed,crc32(0,(const void *)&j->stable,(uInt)sizeof j->stable));
        }
        if(save)pipeline_checkpoint();
    }
    return NULL;
}
static void pipeline_worker_start(void) {
    if(!PW.mode)return;
    const char *manifest=getenv("DM_PSO_MANIFEST");int warm=PW.mode==2&&!G.disable_pso_reuse&&(!manifest||strcmp(manifest,"off"));
    if(manifest&&*manifest&&strcmp(manifest,"off"))pipeline_manifest_load(manifest,warm);
    else{char seed[1200];snprintf(seed,sizeof seed,"%s/pso_manifest.vkbin",G.shader_root);pipeline_manifest_load(seed,warm);}
    pipeline_manifest_load(PW.manifest_path,warm);
    int rc=pthread_create(&PW.thread,NULL,pipeline_worker,NULL);if(rc){PW.mode=0;atomic_store(&PW.warm_pending,0);fprintf(stderr,"[nvn-vulkan] PIPELINE_WORKER unavailable=%d synchronous_fallback=1\n",rc);return;}
    pthread_detach(PW.thread);PW.started=1;
    fprintf(stderr,"[nvn-vulkan] PIPELINE_WARM_START entries=%u queued=%u mode=%d\n",PW.count,atomic_load(&PW.warm_pending),PW.mode);
}
static Pso pipeline_obtain(const PsoKey *key,uint64_t hash,VShader *v,VShader *f) {
    uint64_t t=dm_android_nvn_clock_ns();Pso pso={0};int obtained=0;
    if(PW.started&&PW.mode==2&&!G.disable_pso_reuse&&G.npso<LIMIT_PSOS){
        StablePso stable=pipeline_stable(key);pthread_mutex_lock(&PW.lock);int at=pipeline_find(&stable);
        if(at<0&&PW.count<LIMIT_PSOS){at=(int)PW.count++;PW.jobs[at]=(PipelineJob){.stable=stable,.state=PJ_QUEUED};PW.generation++;}
        if(at>=0){PipelineJob *j=&PW.jobs[at];j->demand=1;j->demand_shader[0]=v;j->demand_shader[1]=f;pthread_cond_signal(&PW.changed);
            int waited=j->state==PJ_QUEUED||j->state==PJ_BUILDING;if(waited)PW.waits++;
            while(j->state==PJ_QUEUED||j->state==PJ_BUILDING)pthread_cond_wait(&PW.changed,&PW.lock);
            if(j->state==PJ_READY){pso=j->pso;j->state=PJ_CLAIMED;obtained=1;PW.claims++;
                fprintf(stderr,"[nvn-vulkan] PIPELINE_USE seed=%d waited=%d wait_ns=%llu compute=%u\n",j->seed,waited,(unsigned long long)(dm_android_nvn_clock_ns()-t),key->compute);}
        }
        pthread_mutex_unlock(&PW.lock);PW.wait_ns+=dm_android_nvn_clock_ns()-t;
    }
    if(!obtained){uint64_t begin=dm_android_nvn_clock_ns();CHECK(pipeline_build(key,v,f,&pso));PW.sync_creates++;
        fprintf(stderr,"[nvn-vulkan] PIPELINE_CREATE source=synchronous compute=%u result=0 build_ns=%llu\n",key->compute,(unsigned long long)(dm_android_nvn_clock_ns()-begin));
        if(PW.started){StablePso stable=pipeline_stable(key);pthread_mutex_lock(&PW.lock);int at=pipeline_find(&stable);
            if(at<0&&PW.count<LIMIT_PSOS){at=(int)PW.count++;PW.jobs[at]=(PipelineJob){.stable=stable,.state=PJ_CLAIMED};}
            PW.generation++;pthread_cond_signal(&PW.changed);pthread_mutex_unlock(&PW.lock);}
    }
    G.profile_pipeline_ns+=dm_android_nvn_clock_ns()-t;return pso_remember(key,hash,pso);
}
int mtl_warm_wait(int timeout) {
    if(timeout<=0||!PW.started||!atomic_load(&PW.warm_pending))return 0;
    uint64_t begin=dm_android_nvn_clock_ns(),until=begin+(uint64_t)timeout*1000000;
    while(atomic_load(&PW.warm_pending)&&dm_android_nvn_clock_ns()<until)usleep(2000);
    return (int)((dm_android_nvn_clock_ns()-begin)/1000000);
}
static void pipeline_frame(uint64_t frame) {
    if(PW.trace)fprintf(stderr,"[nvn-vulkan] PIPELINE_FRAME pid=%d frame=%llu mono_ns=%llu mode=%d cached=%u sync_creates=%llu claims=%llu waits=%llu wait_ns=%llu bg_created=%llu bg_ns=%llu pending=%u\n",getpid(),(unsigned long long)frame,(unsigned long long)dm_android_nvn_clock_ns(),PW.mode,G.npso,(unsigned long long)PW.sync_creates,(unsigned long long)PW.claims,(unsigned long long)PW.waits,(unsigned long long)PW.wait_ns,(unsigned long long)atomic_load(&PW.background_creates),(unsigned long long)atomic_load(&PW.background_ns),atomic_load(&PW.warm_pending));
}
