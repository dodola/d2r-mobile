/* Vulkan counterpart of upstream warm_start / pso_user_record / warm_claim.
 * Record only complete pipelines actually requested; never a shader cross-product.
 * Stable shader names replace process-local shader indices in the disk manifest.
 * One compiler owns pending work. Replay may wait for a required pipeline, but
 * never skips a draw or uses a substitute pipeline. No worker takes G.lock. */
typedef struct {PsoKey key;char shader[2][17];} StablePso;
enum { PJ_QUEUED=1,PJ_BUILDING,PJ_READY,PJ_FAILED,PJ_CLAIMED,PJ_DORMANT };
typedef struct {StablePso stable;Pso pso;int state,demand,seed;VkResult result;} PipelineJob;
static struct {
    pthread_mutex_t lock,store_lock;pthread_cond_t changed;pthread_t thread;
    PipelineJob jobs[LIMIT_PSOS];unsigned count;int mode,started,trace;
    char manifest_path[1200],legacy_path[1200];DmPipelineIdentity identity;
    uint64_t generation,saved_generation,last_save,loaded_bytes;
    uint64_t sync_creates,claims,waits,wait_ns;
    _Atomic uint64_t background_creates,background_ns;
    _Atomic unsigned warm_pending;
} PW={.lock=PTHREAD_MUTEX_INITIALIZER,.store_lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
typedef struct {uint32_t bias[LIMIT_BINDINGS];int32_t draw[3];} PipelineSpecialization;
static VkSpecializationInfo pipeline_specialization(const VShader *s,const uint32_t *bias,const int32_t draw[3],
    PipelineSpecialization *values,VkSpecializationMapEntry *entries) {
    memcpy(values->bias,bias,sizeof values->bias);memset(values->draw,0,sizeof values->draw);
    uint32_t n=0;for(unsigned i=0;i<s->h.nb;i++)if(s->storage_bias.mask&(1ull<<i))entries[n++]=(VkSpecializationMapEntry){s->storage_bias.spec_id[i],i*4,4};
    if(draw)for(unsigned f=0;f<3;f++)if(s->draw_parameter_mask&(1u<<f)){
        values->draw[f]=draw[f];entries[n++]=(VkSpecializationMapEntry){NVN_DRAW_SPEC_BASE+f,offsetof(PipelineSpecialization,draw)+f*4,4};}
    return (VkSpecializationInfo){n,entries,sizeof *values,values};
}
/* This is the single factory for demand compilation and manifest warm-up.
 * Its only inputs are immutable key bytes, shader objects and device handles. */
static VkResult pipeline_build(const PsoKey *k,VShader *v,VShader *f,Pso *out) {
    memset(out,0,sizeof *out);VkDescriptorSetLayout layouts[]={v->layout,f?f->layout:VK_NULL_HANDLE};
    VkPipelineLayoutCreateInfo li={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=f?2u:1u,.pSetLayouts=layouts};
    VkResult result=vkCreatePipelineLayout(G.device,&li,NULL,&out->layout);if(result!=VK_SUCCESS)return result;
    VkSpecializationMapEntry maps[2][LIMIT_BINDINGS+3];PipelineSpecialization values[2];
    VkSpecializationInfo spec[]={pipeline_specialization(v,k->storage_bias[0],k->compute?NULL:k->draw_parameters,&values[0],maps[0]),{0}};
    if(f)spec[1]=pipeline_specialization(f,k->storage_bias[1],NULL,&values[1],maps[1]);
    VkShaderModule vertex=k->compute?shader_module(v,k->null_bindings[0]):v->module;
    if(!k->compute){
        NvnVkVertexAdapt plan={.split=k->vertex_split};uint32_t locations=0;
        for(unsigned l=0;l<16;l++){
            if(v->vertex_inputs[l].type)locations|=1u<<l;
            plan.convert[l]=(k->vertex_unsigned&(1u<<l))?1:(k->vertex_signed&(1u<<l))?2:0;
        }
        /* Match draw's deterministic tail-location allocation in metadata order. */
        for(unsigned i=0;i<v->h.na;i++){unsigned l=v->attr[i][0];if(!(plan.split&(1u<<l)))continue;
            unsigned tail=0;while(tail<32&&(locations&(1u<<tail)))tail++;
            if(tail>=32){vkDestroyPipelineLayout(G.device,out->layout,NULL);memset(out,0,sizeof *out);return VK_ERROR_INITIALIZATION_FAILED;}
            locations|=1u<<tail;plan.tail[l]=tail;
        }
        vertex=vertex_module(v,&plan,k->null_bindings[0]);
    }
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=k->compute?VK_SHADER_STAGE_COMPUTE_BIT:VK_SHADER_STAGE_VERTEX_BIT,.module=vertex,.pName="main",.pSpecializationInfo=spec[0].mapEntryCount?&spec[0]:NULL},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=f?shader_module(f,k->null_bindings[1]):VK_NULL_HANDLE,.pName="main",.pSpecializationInfo=spec[1].mapEntryCount?&spec[1]:NULL}};
    VkRenderPass pass=VK_NULL_HANDLE;
    if(k->compute){
        VkComputePipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,.stage=stages[0],.layout=out->layout};
        pthread_mutex_lock(&PC.cache_lock);result=vkCreateComputePipelines(G.device,G.pipeline_cache,1,&ci,NULL,&out->pipeline);if(result==VK_SUCCESS)atomic_fetch_add(&PC.revision,1);pthread_mutex_unlock(&PC.cache_lock);
    }else{
        const PassKey *p=&k->pass;
        VkSubpassDescription sub={.flags=p->subpass_flags,.inputAttachmentCount=p->input_count,.pInputAttachments=p->input_count?&p->input:NULL,.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=p->color_count,.pColorAttachments=p->color,.pDepthStencilAttachment=p->has_depth?&p->depth:NULL};
        VkInputAttachmentAspectReference input_aspect={.subpass=0,.inputAttachmentIndex=0,.aspectMask=p->input_aspect};
        VkRenderPassInputAttachmentAspectCreateInfo input_info={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,.aspectReferenceCount=1,.pAspectReferences=&input_aspect};
        VkRenderPassCreateInfo ri={.pNext=p->input_count?&input_info:NULL,.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=p->count,.pAttachments=p->attachments,.subpassCount=1,.pSubpasses=&sub,.dependencyCount=p->dependency_count,.pDependencies=p->dependencies};
        result=vkCreateRenderPass(G.device,&ri,NULL,&pass);if(result!=VK_SUCCESS)goto done;
        VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,.vertexBindingDescriptionCount=k->binding_count,.pVertexBindingDescriptions=k->bindings,.vertexAttributeDescriptionCount=k->attribute_count,.pVertexAttributeDescriptions=k->attributes};
        VkPipelineInputAssemblyStateCreateInfo ai={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=k->topology};
        VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
        VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,.polygonMode=VK_POLYGON_MODE_FILL,.depthClampEnable=k->depth_clamp,.cullMode=k->cull,.frontFace=k->front,.lineWidth=1,
            .depthBiasEnable=k->depth_bias.enabled,.depthBiasConstantFactor=k->depth_bias.constant,
            .depthBiasSlopeFactor=k->depth_bias.slope,.depthBiasClamp=k->depth_bias.clamp};
        VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo ds={.flags=k->depth_flags,.sType=VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,.depthTestEnable=k->depth_test,.depthWriteEnable=k->depth_write,.depthCompareOp=k->depth_compare,.stencilTestEnable=k->stencil_test,.front=k->stencil[0],.back=k->stencil[1]};
        VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=k->color_count,.pAttachments=k->blend};
        VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_BLEND_CONSTANTS};
        VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=3,.pDynamicStates=dynamic};
        VkGraphicsPipelineCreateInfo gp={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.flags=k->flags,.stageCount=f?2u:1u,.pStages=stages,.pVertexInputState=&vi,.pInputAssemblyState=&ai,.pViewportState=&vp,.pRasterizationState=&rs,.pMultisampleState=&ms,.pDepthStencilState=&ds,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=out->layout,.renderPass=pass};
        pthread_mutex_lock(&PC.cache_lock);result=vkCreateGraphicsPipelines(G.device,G.pipeline_cache,1,&gp,NULL,&out->pipeline);if(result==VK_SUCCESS)atomic_fetch_add(&PC.revision,1);pthread_mutex_unlock(&PC.cache_lock);
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
    if((k->vertex_split|k->vertex_unsigned|k->vertex_signed)&~0xffffu)return 0;
    if(k->vertex_unsigned&k->vertex_signed)return 0;
    if(k->depth_bias.enabled>1||!isfinite(k->depth_bias.constant)||!isfinite(k->depth_bias.slope)||!isfinite(k->depth_bias.clamp))return 0;
    if(k->shader_variant[0]||k->shader_variant[1]>1||k->depth_clamp>1)return 0;
    if(k->compute)return !s->shader[1][0]&&!k->shader_variant[1];
    const PassKey *p=&k->pass;if(!p->count||p->count>9||p->color_count!=k->color_count||p->color_count>8||p->has_depth>1||p->dependency_count>3||p->input_count>1||(p->input_count&&(!p->has_depth||p->input.attachment>=p->count)))return 0;
    for(unsigned i=0;i<p->color_count;i++)if(p->color[i].attachment!=VK_ATTACHMENT_UNUSED&&p->color[i].attachment>=p->count)return 0;
    if(p->has_depth&&p->depth.attachment>=p->count)return 0;
    return 1;
}
static StablePso pipeline_stable(const PsoKey *key) {
    StablePso s={0};s.key=*key;
    for(unsigned i=0;i<2;i++){if(key->shader[i])memcpy(s.shader[i],G.shaders[(key->shader[i]&0x7fffffffu)-1].key,17);s.key.shader[i]=0;}
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
    if(getenv("DM_DISABLE_PIPELINE_CACHE_DISK"))PW.mode=0;
    PW.trace=getenv("DM_VK_PIPELINE_TRACE")!=NULL;
    uint8_t encoded[NVN_VK_CACHE_KEY_BYTES];nvn_vk_cache_key_encode(encoded,&G.pipeline_cache_key);
    PW.identity=(DmPipelineIdentity){.vendor=G.properties.vendorID,.device=G.properties.deviceID,.driver=G.properties.driverVersion,
        .key_size=sizeof(StablePso),.options=crc32(0,encoded,sizeof encoded)};
    const uint8_t storage_bias=getenv("DM_DISABLE_STORAGE_BIAS")==NULL;
    PW.identity.options=crc32(PW.identity.options,&storage_bias,1);
    memcpy(PW.identity.uuid,G.properties.pipelineCacheUUID,16);
    if(PW.mode){
        size_t index_size=0;uint8_t *index=read_file("shader_index.bin",&index_size);PW.identity.shader_crc=crc32(0,index,(uInt)index_size);free(index);
        /* One user manifest per driver identity: switching between the system driver and Turnip no longer overwrites the
         * other driver's warm-start list. The unsuffixed pso_user.vkbin (shipped seeds) is still read, and rejected by the
         * identity check when it belongs to another driver. */
        uint32_t manifest_id=crc32(0,(const uint8_t *)&PW.identity.vendor,sizeof PW.identity.vendor);
        manifest_id=crc32(manifest_id,(const uint8_t *)&PW.identity.device,sizeof PW.identity.device);
        manifest_id=crc32(manifest_id,(const uint8_t *)&PW.identity.driver,sizeof PW.identity.driver);
        manifest_id=crc32(manifest_id,PW.identity.uuid,sizeof PW.identity.uuid);
        manifest_id=crc32(manifest_id,(const uint8_t *)&PW.identity.options,sizeof PW.identity.options);
        if(snprintf(PW.manifest_path,sizeof PW.manifest_path,"%s/pso_user-%08x.vkbin",dir,manifest_id)>=(int)sizeof PW.manifest_path||
           snprintf(PW.legacy_path,sizeof PW.legacy_path,"%s/pso_user.vkbin",dir)>=(int)sizeof PW.legacy_path)die("pipeline manifest path",0);
    }
    /* Main's validated driver cache is the sole opaque-cache reader/writer. */
    pipeline_cache_start();PW.loaded_bytes=PC.loaded_bytes;
    fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE mode=%d loaded_bytes=%llu shader_crc=%08x key_size=%zu\n",PW.mode,(unsigned long long)PW.loaded_bytes,PW.identity.shader_crc,sizeof(StablePso));
}
typedef struct WarmShader {struct WarmShader *next;VShader shader;} WarmShader;
static WarmShader *pipeline_shaders;
static VShader *pipeline_shader(const char *key,unsigned stage,unsigned variant) {
    if(!*key)return NULL;
    for(WarmShader *p=pipeline_shaders;p;p=p->next)if(!strcmp(p->shader.key,key)&&p->shader.h.stage==stage)return variant?depth_input_shader(&p->shader):&p->shader;
    char path[1200];snprintf(path,sizeof path,"%s/%s.vkmeta",G.shader_root,key);FILE *f=fopen(path,"rb");if(!f)return NULL;
    RHeader h;int ok=fread(&h,1,sizeof h,f)==sizeof h&&h.magic==0x4d4b5644&&h.version==1&&h.stage==stage;fclose(f);if(!ok)return NULL;
    snprintf(path,sizeof path,"%s/%s.spv%s",G.shader_root,key,h.reserved?".gz":"");if(access(path,R_OK))return NULL;
    WarmShader *p=calloc(1,sizeof *p);if(!p)return NULL;shader_load_into(&p->shader,key,stage,1);p->next=pipeline_shaders;pipeline_shaders=p;return variant?depth_input_shader(&p->shader):&p->shader;
}
static void pipeline_checkpoint(void) {
    pthread_mutex_lock(&PW.store_lock);
    pthread_mutex_lock(&PW.lock);uint64_t generation=PW.generation;unsigned count=PW.count;
    StablePso *keys=malloc(count*sizeof(*keys));if(keys)for(unsigned i=0;i<count;i++)keys[i]=PW.jobs[i].stable;
    pthread_mutex_unlock(&PW.lock);
    int manifest_ok=!count||(keys&&dm_pipeline_write(PW.manifest_path,2,&PW.identity,keys,count*sizeof(*keys)));free(keys);
    pthread_mutex_lock(&PW.lock);PW.last_save=dm_android_nvn_clock_ns();if(manifest_ok)PW.saved_generation=generation;pthread_mutex_unlock(&PW.lock);
    pthread_mutex_unlock(&PW.store_lock);
    fprintf(stderr,"[nvn-vulkan] PIPELINE_STORE entries=%u manifest_ok=%d generation=%llu\n",count,manifest_ok,(unsigned long long)generation);
}
static void pipeline_manifest_flush(void) {
    pthread_mutex_lock(&PW.lock);int save=PW.started&&PW.mode&&PW.generation!=PW.saved_generation;pthread_mutex_unlock(&PW.lock);
    if(save)pipeline_checkpoint();
}
static void *pipeline_worker(void *unused) {
    (void)unused;
    for(;;){
        pthread_mutex_lock(&PW.lock);int at=-1;
        for(unsigned i=0;i<PW.count;i++)if(PW.jobs[i].state==PJ_QUEUED&&(at<0||PW.jobs[i].demand)){at=(int)i;if(PW.jobs[i].demand)break;}
        uint64_t now=dm_android_nvn_clock_ns();int save=PW.generation!=PW.saved_generation&&now-PW.last_save>=5000000000ull;
        if(at>=0){PW.jobs[at].state=PJ_BUILDING;}
        if(at<0&&!save){struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_sec++;pthread_cond_timedwait(&PW.changed,&PW.lock,&deadline);pthread_mutex_unlock(&PW.lock);continue;}
        pthread_mutex_unlock(&PW.lock);
        if(at>=0){PipelineJob *j=&PW.jobs[at];uint64_t t=dm_android_nvn_clock_ns();
            VShader *v=pipeline_shader(j->stable.shader[0],j->stable.key.compute?5u:0u,j->stable.key.shader_variant[0]);
            VShader *f=pipeline_shader(j->stable.shader[1],1,j->stable.key.shader_variant[1]);
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
    pipeline_manifest_load(PW.legacy_path,warm);
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
        if(at>=0){PipelineJob *j=&PW.jobs[at];j->demand=1;pthread_cond_signal(&PW.changed);
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
