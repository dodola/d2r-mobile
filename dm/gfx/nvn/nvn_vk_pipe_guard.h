/* 管线崩溃记录：完整状态键、驱动/着色器身份隔离、逐线程持久记号。参考 upstream 2428e5b。
 * Pipeline crash records: full state keys, driver/shader identity isolation, durable per-thread markers. Based on upstream 2428e5b. */
#pragma once
#include <pthread.h>
#include "nvn_vk_pipeline_store.h"
#define DM_PIPE_GUARD_LIMIT 256
#define DM_PIPE_GUARD_SLOTS 16
/* 本头在 StablePso 定义后引用；测试使用同样的持久键契约。
 * Included after StablePso is defined; tests exercise the same persistent-key contract. */
typedef struct {StablePso key;uint32_t crashes,quarantined;} DmPipeFailure;
static struct {
    pthread_mutex_t lock;pthread_key_t slot;int initialized,enabled,degrade,retry;unsigned next_slot;
    DmPipelineIdentity identity;char path[1200],base[1200];DmPipeFailure failures[DM_PIPE_GUARD_LIMIT];unsigned count;
} PG={.lock=PTHREAD_MUTEX_INITIALIZER};
static int pipe_guard_find(const StablePso *key) {
    for(unsigned i=0;i<PG.count;i++)if(!memcmp(key,&PG.failures[i].key,sizeof *key))return (int)i;
    return -1;
}
static void pipe_guard_store(void) {
    /* 非空哨兵使空名单同样能原子落盘，避免旧名单在重启后复活。
     * A nonempty sentinel allows atomic storage of an empty list, preventing old entries from returning on restart. */
    DmPipeFailure empty={0};
    if(!dm_pipeline_write(PG.path,4,&PG.identity,PG.count?PG.failures:&empty,PG.count?PG.count*sizeof(*PG.failures):sizeof empty))
        fprintf(stderr,"[nvn-vulkan] PIPE_GUARD store_failed=1\n");
}
static void pipe_guard_marker(char path[1400],unsigned slot) {snprintf(path,1400,"%s.inflight.%u",PG.base,slot);}
static void pipe_guard_init(const char *dir,const DmPipelineIdentity *identity) {
    if(PG.initialized||!dir||!*dir)return;PG.initialized=1;PG.identity=*identity;
    uint32_t hash=crc32(0,(const void*)identity,sizeof *identity);
    if(snprintf(PG.base,sizeof PG.base,"%s/pipe-guard-%08x",dir,hash)>=(int)sizeof PG.base)return;
    if(snprintf(PG.path,sizeof PG.path,"%s/pipe-guard-%08x.vkbin",dir,hash)>=(int)sizeof PG.path)return;
    char reset[1400],option[1400];snprintf(reset,sizeof reset,"%s/gfx-pipe-reset",dir);
    int clear=access(reset,F_OK)==0;if(clear)unlink(reset);
    PG.degrade=getenv("DM_VK_PIPE_DEGRADE")&&atoi(getenv("DM_VK_PIPE_DEGRADE"));
    PG.retry=getenv("DM_VK_PIPE_RETRY")&&atoi(getenv("DM_VK_PIPE_RETRY"));
    snprintf(option,sizeof option,"%s/gfx-pipe-degrade",dir);FILE *o=fopen(option,"rb");if(o){PG.degrade=fgetc(o)=='1';fclose(o);}
    size_t bytes=0;DmPipeFailure *saved=clear?NULL:dm_pipeline_read(PG.path,4,identity,sizeof PG.failures,&bytes);
    if(saved&&bytes%sizeof(*saved)==0)for(unsigned i=0;i<bytes/sizeof(*saved);i++)if(saved[i].crashes&&pipeline_stable_valid(&saved[i].key))PG.failures[PG.count++]=saved[i];
    free(saved);StablePso seen[DM_PIPE_GUARD_SLOTS];unsigned nseen=0;
    for(unsigned i=0;i<DM_PIPE_GUARD_SLOTS;i++){
        char marker[1400];pipe_guard_marker(marker,i);size_t size=0;
        StablePso *key=clear?NULL:dm_pipeline_read(marker,3,identity,sizeof(StablePso),&size);unlink(marker);
        if(!key)continue;
        if(size!=sizeof(*key)){free(key);continue;}
        int duplicate=0;for(unsigned k=0;k<nseen;k++)if(!memcmp(&seen[k],key,sizeof *key))duplicate=1;
        if(size==sizeof(*key)&&pipeline_stable_valid(key)&&!duplicate){
            seen[nseen++]=*key;int at=pipe_guard_find(key);
            if(at<0&&PG.count<DM_PIPE_GUARD_LIMIT){at=(int)PG.count++;PG.failures[at]=(DmPipeFailure){.key=*key};}
            if(at>=0){DmPipeFailure *f=&PG.failures[at];if(f->crashes<2)f->crashes++;f->quarantined=f->crashes>=2;
                fprintf(stderr,"[nvn-vulkan] PIPE_GUARD abandoned_boots=%u quarantined=%u compute=%u key=%08x\n",f->crashes,f->quarantined,key->key.compute,(uint32_t)crc32(0,(const void*)key,sizeof *key));}
        }
        free(key);
    }
    /* 未隔离的累计只允许相邻启动；中间没有遗留记号即重新计数。
     * Unquarantined counts require adjacent boots; a boot without a matching marker breaks the streak. */
    for(unsigned i=0;i<PG.count;){
        int repeated=0;for(unsigned k=0;k<nseen;k++)if(!memcmp(&seen[k],&PG.failures[i].key,sizeof(StablePso)))repeated=1;
        if(!PG.failures[i].quarantined&&!repeated)PG.failures[i]=PG.failures[--PG.count];else i++;
    }
    if(pthread_key_create(&PG.slot,NULL))return;PG.enabled=1;pipe_guard_store();
    fprintf(stderr,"[nvn-vulkan] PIPE_GUARD identity=%08x entries=%u degrade=%d retry=%d reset=%d\n",hash,PG.count,PG.degrade,PG.retry,clear);
}
static int pipe_guard_quarantined(const StablePso *key) {
    if(!PG.enabled||PG.retry)return 0;
    pthread_mutex_lock(&PG.lock);int at=pipe_guard_find(key),blocked=at>=0&&PG.failures[at].quarantined;pthread_mutex_unlock(&PG.lock);return blocked;
}
static unsigned pipe_guard_slot(void) {
    uintptr_t slot=(uintptr_t)pthread_getspecific(PG.slot);
    if(!slot){pthread_mutex_lock(&PG.lock);slot=++PG.next_slot;pthread_mutex_unlock(&PG.lock);pthread_setspecific(PG.slot,(void*)slot);}
    return (unsigned)(slot-1);
}
static void pipe_guard_begin(const StablePso *key) {
    if(!PG.enabled)return;unsigned slot=pipe_guard_slot();if(slot>=DM_PIPE_GUARD_SLOTS)return;
    char marker[1400];pipe_guard_marker(marker,slot);
    if(!dm_pipeline_write(marker,3,&PG.identity,key,sizeof *key))fprintf(stderr,"[nvn-vulkan] PIPE_GUARD marker_failed=1 slot=%u\n",slot);
}
static void pipe_guard_end(const StablePso *key,int32_t result) {
    if(!PG.enabled)return;char marker[1400];unsigned slot=pipe_guard_slot();pipe_guard_marker(marker,slot);unlink(marker);
    pthread_mutex_lock(&PG.lock);int at=pipe_guard_find(key);
    if(!result&&at>=0){PG.failures[at]=PG.failures[--PG.count];pipe_guard_store();}
    /* 只有 INVALID_SHADER 是确定的模块错误；OOM / DEVICE_LOST / UNKNOWN 只诊断，不持久拉黑。
     * Only INVALID_SHADER is a definitive module error; OOM / DEVICE_LOST / UNKNOWN are diagnosed without persistent quarantine. */
    if(result==-1000012000){
        if(at<0&&PG.count<DM_PIPE_GUARD_LIMIT)at=(int)PG.count++;
        if(at>=0){PG.failures[at]=(DmPipeFailure){.key=*key,.crashes=2,.quarantined=1};pipe_guard_store();}
    }
    pthread_mutex_unlock(&PG.lock);
    if(result)fprintf(stderr,"[nvn-vulkan] PIPE_GUARD result=%d permanent=%d compute=%u\n",result,result==-1000012000,key->key.compute);
}
