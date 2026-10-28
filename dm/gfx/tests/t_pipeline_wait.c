/* 运行生产需求等待函数，工人延迟超过原 3ms 预算，验证绘制仍得到完整管线。
 * Exercise the production demand-wait function with a worker slower than the former 3ms budget, preserving the demanded draw. */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
typedef struct {unsigned compute;} PsoKey;
typedef struct {PsoKey key;} StablePso;
typedef struct {uintptr_t pipeline;} Pso;
typedef int VShader;
typedef int VkResult;
enum {VK_SUCCESS=0,VK_ERROR_INVALID_SHADER_NV=-1000012000};
enum {PJ_QUEUED,PJ_BUILDING,PJ_READY,PJ_FAILED,PJ_CLAIMED,LIMIT_PSOS=8};
typedef struct {StablePso stable;int state,demand,seed;Pso pso;} PipelineJob;
static struct {pthread_mutex_t lock;pthread_cond_t changed;int started,mode;unsigned count;PipelineJob jobs[8];uint64_t generation,waits,claims,wait_ns,sync_creates;} PW={.lock=PTHREAD_MUTEX_INITIALIZER,.changed=PTHREAD_COND_INITIALIZER};
static struct {int disable_pso_reuse;unsigned npso;uint64_t profile_pipeline_ns;} G;
static struct {int degrade;} PG;
static int builds;
static uint64_t dm_android_nvn_clock_ns(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static StablePso pipeline_stable(const PsoKey *k){return (StablePso){*k};}
static int pipeline_find(const StablePso *s){for(unsigned i=0;i<PW.count;i++)if(!memcmp(s,&PW.jobs[i].stable,sizeof *s))return i;return -1;}
static int pipeline_build(const PsoKey *k,VShader *v,VShader *f,Pso *p){(void)k;(void)v;(void)f;builds++;p->pipeline=456;return 0;}
static int pipe_guard_quarantined(const StablePso *s){(void)s;return 0;}
static Pso pso_remember(const PsoKey *k,uint64_t h,Pso p){(void)k;(void)h;return p;}
static void die(const char *s,int e){(void)s;(void)e;assert(0);}
#define CHECK(x) assert((x)==0)
#include "../nvn/nvn_vk_pipeline_obtain.inc"
static void *worker(void *unused){
    (void)unused;pthread_mutex_lock(&PW.lock);
    while(!PW.count||!PW.jobs[0].demand)pthread_cond_wait(&PW.changed,&PW.lock);
    PW.jobs[0].state=PJ_BUILDING;pthread_mutex_unlock(&PW.lock);
    usleep(30000);pthread_mutex_lock(&PW.lock);
    PW.jobs[0].pso.pipeline=123;PW.jobs[0].state=PJ_READY;
    pthread_cond_broadcast(&PW.changed);pthread_mutex_unlock(&PW.lock);return NULL;
}
int main(void){
    for(unsigned compute=0;compute<2;compute++){
        PW.count=0;PW.started=1;PW.mode=2;pthread_t t;assert(!pthread_create(&t,NULL,worker,NULL));
        PsoKey key={compute};int skipped=99;uint64_t start=dm_android_nvn_clock_ns();
        Pso p=pipeline_obtain(&key,0,NULL,NULL,&skipped);
        assert(p.pipeline==123&&!skipped&&dm_android_nvn_clock_ns()-start>=20000000&&!builds);
        assert(!pthread_join(t,NULL));
    }
    PW.started=0;PsoKey key={0};assert(pipeline_obtain(&key,0,NULL,NULL,NULL).pipeline==456&&builds==1);
    puts("PIPELINE_WAIT PASS graphics compute delayed_worker synchronous_fallback no_budget_skip");
}
