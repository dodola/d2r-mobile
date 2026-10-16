/* Included after backend state. Vulkan cache access stays under G.lock; the
 * writer only owns an immutable byte snapshot and never calls Vulkan. */
#include "nvn_vk_pipeline_cache_io.h"
typedef struct {
    NvnVkPipelineCacheKey key;
    char path[1200];
    void *data;size_t bytes;uint64_t revision;
    _Atomic int done;int written;
} PipelineCacheWrite;
static struct {
    PFN_vkGetPipelineCacheData get;
    char path[1200];
    uint64_t revision,saved_revision,next_ns;
    PipelineCacheWrite *pending;pthread_t thread;
} PC;

static void *pipeline_cache_writer(void *opaque) {
    PipelineCacheWrite *job=opaque;
    job->written=nvn_vk_cache_write(job->path,&job->key,job->data,job->bytes);
    fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE_SAVE ok=%d bytes=%zu revision=%llu\n",
        job->written,job->bytes,(unsigned long long)job->revision);
    atomic_store_explicit(&job->done,1,memory_order_release);return NULL;
}
static void pipeline_cache_join(int wait) {
    PipelineCacheWrite *job=PC.pending;
    if(!job||(!wait&&!atomic_load_explicit(&job->done,memory_order_acquire)))return;
    pthread_join(PC.thread,NULL);
    if(job->written)PC.saved_revision=job->revision;
    free(job->data);free(job);PC.pending=NULL;
}
static void pipeline_cache_checkpoint(int force) {
    pipeline_cache_join(force);
    if(!G.pipeline_cache||!PC.get||!PC.path[0]||PC.pending||PC.saved_revision==PC.revision)return;
    uint64_t now=dm_android_nvn_clock_ns();if(!force&&now<PC.next_ns)return;
    PC.next_ns=now+30000000000ull;
    size_t bytes=0;VkResult result=PC.get(G.device,G.pipeline_cache,&bytes,NULL);
    if(result!=VK_SUCCESS||bytes<32||bytes>NVN_VK_CACHE_MAX_BYTES){
        fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE_SNAPSHOT skipped result=%d bytes=%zu\n",result,bytes);return;}
    PipelineCacheWrite *job=calloc(1,sizeof(*job));if(!job)return;
    job->data=malloc(bytes);if(!job->data){free(job);return;}
    result=PC.get(G.device,G.pipeline_cache,&bytes,job->data);
    if(result!=VK_SUCCESS||!nvn_vk_cache_blob_valid(job->data,bytes,&G.pipeline_cache_key)){
        free(job->data);free(job);return;}
    job->key=G.pipeline_cache_key;strcpy(job->path,PC.path);job->bytes=bytes;job->revision=PC.revision;
    if(pthread_create(&PC.thread,NULL,pipeline_cache_writer,job)){free(job->data);free(job);return;}
    PC.pending=job;
}
void dm_vk_pipeline_cache_save(void) {
    pthread_mutex_lock(&G.lock);PC.next_ns=0;pipeline_cache_checkpoint(0);pthread_mutex_unlock(&G.lock);
}
/* Explicit flush for lifecycle boundaries and regression tools. Android can kill
 * a process without atexit, so normal rendering also checkpoints periodically. */
void dm_vk_pipeline_cache_flush(void) {
    pthread_mutex_lock(&G.lock);pipeline_cache_checkpoint(1);pipeline_cache_join(1);
    fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE_STATS pipelines=%llu compile_ns=%llu\n",
        (unsigned long long)PC.revision,(unsigned long long)G.profile_pipeline_ns);
    pthread_mutex_unlock(&G.lock);
}
static void pipeline_cache_start(void) {
    if(getenv("DM_DISABLE_PIPELINE_CACHE"))return;
    void *seed=NULL;size_t seed_bytes=0;int status=0;
    PC.get=(PFN_vkGetPipelineCacheData)dm_vk_get(G.instance,"vkGetPipelineCacheData");
    const char *override=getenv("DM_VK_PIPELINE_CACHE_DIR");
    if(!getenv("DM_DISABLE_PIPELINE_CACHE_DISK")&&PC.get&&(override||G.shader_root[0])){
        char directory[1100];int n=override?snprintf(directory,sizeof directory,"%s",override):
            snprintf(directory,sizeof directory,"%s/../pipeline-cache",G.shader_root);
        struct stat st;
        if(n>0&&(size_t)n<sizeof directory&&(!mkdir(directory,0700)||errno==EEXIST)&&
            !stat(directory,&st)&&S_ISDIR(st.st_mode)){
            n=snprintf(PC.path,sizeof PC.path,"%s/%016llx.bin",directory,
                (unsigned long long)nvn_vk_cache_key_hash(&G.pipeline_cache_key));
            if(n<=0||(size_t)n>=sizeof PC.path)PC.path[0]=0;
        }
        if(PC.path[0])status=nvn_vk_cache_read(PC.path,&G.pipeline_cache_key,&seed,&seed_bytes);
    }
    VkPipelineCacheCreateInfo info={.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize=seed_bytes,.pInitialData=seed};
    VkResult result=vkCreatePipelineCache(G.device,&info,NULL,&G.pipeline_cache);
    if(result!=VK_SUCCESS&&seed_bytes){
        fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE_REJECT result=%d retry=empty\n",result);
        info.initialDataSize=0;info.pInitialData=NULL;
        result=vkCreatePipelineCache(G.device,&info,NULL,&G.pipeline_cache);status=-3;
    }
    free(seed);
    if(result!=VK_SUCCESS){G.pipeline_cache=VK_NULL_HANDLE;PC.path[0]=0;}
    fprintf(stderr,"[nvn-vulkan] PIPELINE_CACHE_LOAD status=%d seed_bytes=%zu enabled=%d disk=%d\n",
        status,seed_bytes,G.pipeline_cache!=VK_NULL_HANDLE,PC.path[0]!=0);
    PC.next_ns=dm_android_nvn_clock_ns()+2000000000ull;
    if(PC.path[0])atexit(dm_vk_pipeline_cache_flush);
}
