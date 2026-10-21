/* Slot-local immutable Vulkan objects. References can survive completed work,
 * but not destruction of their source images. All operations hold G.lock. */
static _Atomic int resource_override=-1,resource_applied;
static uint64_t resource_control_sequence;
void dm_vk_resource_config(int enabled) {atomic_store(&resource_override,enabled<0?-1:!!enabled);}
int dm_vk_resource_applied(void) {return atomic_load(&resource_applied);}
static void resource_cache_clear(VkSubmission *ctx) {
    if(ctx->phase==SUBMISSION_INFLIGHT)die("resource cache clear before fence",ctx->epoch);
    if(ctx->npass||ctx->nview)G.resource_flushes++;
    /* Framebuffers own references to the cached views. */
    for(int i=0;i<ctx->npass;i++){
        vkDestroyFramebuffer(G.device,ctx->framebuffers[i],NULL);
        if(ctx->pass_owned[i])vkDestroyRenderPass(G.device,ctx->passes[i],NULL);
    }
    for(int i=0;i<ctx->nview;i++)vkDestroyImageView(G.device,ctx->temporary_views[i],NULL);
    ctx->npass=ctx->nview=0;ctx->retained_passes=ctx->retained_views=0;memset(ctx->view_table,0,sizeof ctx->view_table);
}
static void resource_cache_retire(VkSubmission *ctx) {
    if(!ctx->retain_resources)resource_cache_clear(ctx);
    else if(ctx->npass||ctx->nview)G.resource_kept++;
}
static void resource_control_poll(void) {
    const char *path=getenv("DM_VK_RESOURCE_CONTROL");if(!path)return;
    static uint64_t last;uint64_t now=dm_android_nvn_clock_ns();if(now-last<100000000)return;last=now;
    FILE *f=fopen(path,"r");if(!f)return;
    char text[96]={0};size_t size=fread(text,1,sizeof text-1,f);int tail=fgetc(f),error=ferror(f);fclose(f);
    if(!size||error||tail!=EOF||memchr(text,0,size))return;
    uint64_t numbers[2]={0};const char *p=text;
    for(unsigned field=0;field<2;field++){
        if(*p<'0'||*p>'9')return;
        uint64_t limit=field?1:UINT64_MAX;
        do {unsigned digit=(unsigned)(*p++-'0');if(digit>limit||numbers[field]>(limit-digit)/10)return;numbers[field]=numbers[field]*10+digit;}while(*p>='0'&&*p<='9');
        if(!field&&*p!=' '&&*p!='\t')return;
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
    }
    if(*p||numbers[0]<=resource_control_sequence)return;
    resource_control_sequence=numbers[0];dm_vk_resource_config((int)numbers[1]);
    fprintf(stderr,"[nvn-vulkan] RESOURCE_CONTROL pid=%d sequence=%llu enabled=%u mono_ns=%llu\n",getpid(),
        (unsigned long long)numbers[0],(unsigned)numbers[1],(unsigned long long)now);
}
static void resource_cache_begin(VkSubmission *ctx) {
    resource_control_poll();int target=atomic_load(&resource_override);
    ctx->retain_resources=!G.disable_resource_cache&&(target<0?G.retain_resources:target);
    /* Trim only at the slot's completed fence, before recording anything new.
     * A single large submission retains the original LIMIT_WRITES bound. */
    if(!ctx->retain_resources||ctx->npass>RETAINED_PASS_LIMIT||ctx->nview>RETAINED_VIEW_LIMIT)resource_cache_clear(ctx);
    ctx->retained_passes=ctx->npass;ctx->retained_views=ctx->nview;
    if(atomic_exchange(&resource_applied,ctx->retain_resources)!=ctx->retain_resources)
        fprintf(stderr,"[nvn-vulkan] RESOURCE_MODE pid=%d sequence=%llu enabled=%d epoch=%llu\n",getpid(),
            (unsigned long long)resource_control_sequence,ctx->retain_resources,(unsigned long long)ctx->epoch);
}
void dm_vk_resource_stats(uint64_t out[8]) {
    pthread_mutex_lock(&G.lock);
    out[0]=G.resource_views;out[1]=G.resource_passes;out[2]=G.resource_hits;
    out[3]=G.resource_flushes;out[4]=G.resource_kept;
    out[5]=(uint64_t)G.submission_slot.npass+G.submission_slot2.npass;
    out[6]=(uint64_t)G.submission_slot.nview+G.submission_slot2.nview;
    out[7]=(uint64_t)atomic_load(&resource_applied);
    pthread_mutex_unlock(&G.lock);
}
