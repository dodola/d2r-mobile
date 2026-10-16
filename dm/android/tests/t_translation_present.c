/* Compile the actual Android NVN adapter against a controllable backend.
 * Backend clocks/fences are real host atomics, not a Vulkan/driver simulation. */
#include <assert.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include "../../gfx/nvn/nvn_obj.h"
#include "../../gfx/nvn/nvn_pace.h"
#include "../nvn_platform_clock.h"
#include "../nvn_translation_sync.h"

#define NVN(name,ret,...) static ret h_##name(__VA_ARGS__)
#define NVN_NFUNC 1
static struct {void *fn;} nvn_slot[1];
static void *nvn_real[1];
static const char *nvn_names[1]={"test"};
static unsigned g_nsem;
static void nvn_init_slots(void) {}
static void nvn_unimpl_entry(void) {}
static void nvn_prof_entry(void) {}
#define LOG(...) do {fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);} while(0)
__attribute__((noreturn)) static void die(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();
}
static uint64_t now_ns(void) {struct timespec t;assert(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
/* Real snapshot cache is shared with Apple and compiled by the NDK build.
 * This fixture isolates the adapter's use of that immutable snapshot API. */
static tex_t texture_snapshot;
static const tex_t *snap_tex(const NVNtexture *t) {texture_snapshot=*(const tex_t *)t;return &texture_snapshot;}
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static int blocked,started,release_job,block_on_event;
static pthread_t replay_thread,hint_thread;
static unsigned begins,execs,presents,hint_begins,hint_ends;
static _Atomic uint64_t backend_fence,completed,wait_budget;
static _Atomic int paused,drains;
static _Atomic unsigned cache_saves;
static _Atomic uint64_t notified_frame;
static _Atomic unsigned idle_begins,idle_ends;
static int expected_crop[4];
static void stall(void) {
    pthread_mutex_lock(&gate);
    if(blocked){started=1;pthread_cond_broadcast(&cv);while(!release_job)pthread_cond_wait(&cv,&gate);blocked=0;}
    pthread_mutex_unlock(&gate);
}
static void arm(void) {pthread_mutex_lock(&gate);blocked=1;started=release_job=0;pthread_mutex_unlock(&gate);}
static void await_start(void) {pthread_mutex_lock(&gate);while(!started)pthread_cond_wait(&cv,&gate);pthread_mutex_unlock(&gate);}
static void release(void) {pthread_mutex_lock(&gate);release_job=1;pthread_cond_broadcast(&cv);pthread_mutex_unlock(&gate);}
static void delay(uint64_t ns) {struct timespec t={(time_t)(ns/1000000000),(long)(ns%1000000000)};nanosleep(&t,NULL);}
void mtl_sub_begin(void) {replay_thread=pthread_self();begins++;if(!block_on_event)stall();}
static void execute(const uint8_t *p) {
    for(;;){op_hdr h;memcpy(&h,p,sizeof h);const uint8_t *a=p+sizeof h;
        switch(h.op){
        case OP_BEGIN:break;
        case OP_END:return;
        case OP_CALL:{const uint8_t *next;memcpy(&next,a,8);execute(next);break;}
        case OP_JUMP:memcpy(&p,a,8);continue;
        case OP_FENCE_SYNC:{p_sync s;memcpy(&s,a,sizeof s);dm_nvn_sync_publish(s.sync,atomic_load(&backend_fence)+7);break;}
        case OP_WAIT_EVENT:atomic_fetch_add(&backend_fence,7);atomic_store(&completed,atomic_load(&backend_fence));stall();break;
        default:assert(0);
        }p+=h.size;
    }
}
void mtl_exec(const uint8_t *handle) {execute(handle);execs++;}
void mtl_sub_index(const uint64_t *handles,int n) {(void)handles;(void)n;}
void mtl_sub_commit_async(void) {atomic_fetch_add(&backend_fence,7);}
uint64_t mtl_last_fence(void) {return atomic_load(&backend_fence);}
int mtl_wait(uint64_t fence,uint64_t timeout) {
    if(timeout)atomic_store(&wait_budget,timeout);
    uint64_t start=now_ns();
    while(atomic_load(&completed)<fence){if(timeout!=UINT64_MAX&&now_ns()-start>=timeout)return 0;delay(100000);}
    return 1;
}
void mtl_finish(void) {atomic_store(&completed,atomic_load(&backend_fence));atomic_fetch_add(&drains,1);}
void mtl_drain(void) {mtl_finish();}
void dm_vk_pipeline_cache_save(void) {
    assert(atomic_load(&paused));
    assert(atomic_load(&completed)==atomic_load(&backend_fence));
    atomic_fetch_add(&cache_saves,1);
}
uint64_t mtl_present(const tex_t *t,double period,const int crop[4]) {
    (void)period;stall();assert(t->serial==42);assert(!memcmp(crop,expected_crop,sizeof expected_crop));
    atomic_fetch_add(&backend_fence,3);return ++presents;
}
void mtl_set_frame(uint64_t frame) {assert(frame==presents);}
int mtl_has_window(void) {return 1;}
void *dm_vk_native_window(void) {return (void *)1;}
void dm_android_nvn_schedule_thread(const char *role) {(void)role;}
void dm_android_nvn_sleep_until(uint64_t deadline) {(void)deadline;}
void dm_android_nvn_presented(uint64_t frame) {atomic_store(&notified_frame,frame);}
void dm_android_nvn_work_begin(void) {hint_thread=pthread_self();hint_begins++;}
void dm_android_nvn_work_check_thread(void) {if(hint_begins>hint_ends)assert(pthread_equal(hint_thread,pthread_self()));}
void dm_android_nvn_work_end(uint64_t frame) {(void)frame;assert(hint_begins>hint_ends);assert(pthread_equal(hint_thread,pthread_self()));hint_ends++;}
void dm_android_nvn_work_reset(int foreground) {if(!foreground)assert(!blocked);atomic_store(&paused,!foreground);}
uint64_t dm_android_nvn_work_wait_begin(enum dm_android_nvn_wait_kind kind) {
    assert(kind==DM_NVN_WAIT_TRANSLATION_QUEUE);atomic_fetch_add(&idle_begins,1);return 1;
}
void dm_android_nvn_work_wait_end(uint64_t token) {assert(token==1);atomic_fetch_add(&idle_ends,1);}
#include "../nvn_platform_present.inc"

static NVNqueue queue;
static NVNcommandHandle handles[1];
static void *submit(void *unused) {(void)unused;h_nvnQueueSubmitCommands(&queue,1,handles);return NULL;}
static void *suspend_gfx(void *unused) {(void)unused;dm_gfx_suspend(1);return NULL;}
static void *release_later(void *unused) {(void)unused;delay(20000000);release();return NULL;}
static void *gpu_later(void *unused) {(void)unused;delay(10000000);atomic_store(&completed,atomic_load(&backend_fence));return NULL;}
static void *acquire(void *p) {int index;h_nvnWindowAcquireTexture(p,NULL,&index);return NULL;}
int main(int argc,char **argv) {
    int synchronous=argc>1&&atoi(argv[1]);if(synchronous)setenv("DM_NVN_SYNC","1",1);else unsetenv("DM_NVN_SYNC");
    nvn_present_config(0,0);
    op_hdr command[2]={{OP_BEGIN,8},{OP_END,8}},bad={OP_COUNT_,8};handles[0]=(NVNcommandHandle)(uintptr_t)&command;
    arm();pthread_t producer;assert(!pthread_create(&producer,NULL,submit,NULL));await_start();
    /* Submit copies the caller's handle array before any replay, in both modes. */
    handles[0]=(NVNcommandHandle)(uintptr_t)&bad;
    sync_t sync={.magic=MAGIC('S','Y','N','C'),.fenced=1,.fence=0};
    /* An unrelated stalled submit cannot delay an already completed fence. */
    uint64_t before=now_ns();assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);assert(now_ns()-before<100000000);
    release();assert(!pthread_join(producer,NULL));x_drain();assert(begins==1&&execs==1);
    assert(pthread_equal(replay_thread,producer)==synchronous);
    android_queue_fence(&sync);x_drain();assert(sync.fence==7);
    assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
    pthread_t gpu;assert(!pthread_create(&gpu,NULL,gpu_later,NULL));
    assert(android_sync_wait(&sync,UINT64_MAX)==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED);assert(!pthread_join(gpu,NULL));
    assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);

    if(!synchronous){
        /* Finite timeout covers CPU translation plus GPU completion together. */
        handles[0]=(NVNcommandHandle)(uintptr_t)&command;arm();h_nvnQueueSubmitCommands(&queue,1,handles);await_start();android_queue_fence(&sync);
        assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        pthread_t rel;assert(!pthread_create(&rel,NULL,release_later,NULL));before=now_ns();
        assert(android_sync_wait(&sync,80000000)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        assert(now_ns()-before>=80000000 && now_ns()-before<300000000);
        assert(atomic_load(&wait_budget)>0&&atomic_load(&wait_budget)<70000000);assert(!pthread_join(rel,NULL));

        /* A fence followed by a CPU-dependent event in the SAME submit must
         * become waitable before the submit's translation job is finished. */
        struct {op_hdr begin,fence;p_sync s;op_hdr event,end;} stream={
            {OP_BEGIN,8},{OP_FENCE_SYNC,8+sizeof(p_sync)},{&sync,0,0},{OP_WAIT_EVENT,8},{OP_END,8}};
        handles[0]=(NVNcommandHandle)(uintptr_t)&stream;block_on_event=1;arm();
        h_nvnQueueSubmitCommands(&queue,1,handles);await_start();
        assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        assert(!dm_nvn_translation_wait(&android_xqueue,dm_nvn_translation_tail(&android_xqueue),0));
        release();x_drain();block_on_event=0;

        /* Two fences on one sync in one job must not publish the second one
         * when only the first has executed. Include CALL and JUMP traversal. */
        struct {op_hdr fence;p_sync s;op_hdr end;} second={{OP_FENCE_SYNC,8+sizeof(p_sync)},{&sync,0,0},{OP_END,8}};
        struct {op_hdr begin,fence;p_sync s;op_hdr event,call;const void *next;op_hdr end;} repeated={
            {OP_BEGIN,8},{OP_FENCE_SYNC,8+sizeof(p_sync)},{&sync,0,0},{OP_WAIT_EVENT,8},{OP_CALL,16},&second,{OP_END,8}};
        struct {op_hdr jump;const void *next;} jump={{OP_JUMP,16},&repeated};
        handles[0]=(NVNcommandHandle)(uintptr_t)&jump;block_on_event=1;arm();
        h_nvnQueueSubmitCommands(&queue,1,handles);await_start();
        assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED);
        release();x_drain();block_on_event=0;mtl_finish();
        assert(android_sync_wait(&sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED);
        assert(atomic_load(&sync.pending)==atomic_load(&sync.translated));
    }

    tex_t texture={.serial=42};NVNwindow window;win_t *w=(void *)&window;
    memset(w,0,sizeof(*w));w->ntex=1;w->tex[0]=(NVNtexture *)&texture;
    int index;h_nvnWindowAcquireTexture(&window,NULL,&index);x_drain();
    expected_crop[0]=3;expected_crop[1]=4;expected_crop[2]=100;expected_crop[3]=80;
    memcpy(w->crop,expected_crop,sizeof expected_crop);
    if(!synchronous){
        /* Hold an earlier submit so present parameters can be mutated after enqueue. */
        arm();h_nvnQueueSubmitCommands(&queue,0,NULL);await_start();
        h_nvnQueuePresentTexture(&queue,&window,0);texture.serial=99;memset(w->crop,0,sizeof w->crop);
        pthread_t next;assert(!pthread_create(&next,NULL,acquire,&window));
        delay(5000000);assert(dm_nvn_frame_count()==0);release();assert(!pthread_join(next,NULL));
    }else h_nvnQueuePresentTexture(&queue,&window,0);
    x_drain();assert(presents==1&&dm_nvn_frame_count()==1&&atomic_load(&notified_frame)==1);
    assert(hint_ends==1);
    h_nvnWindowFinalize(&window);assert(!w->magic);assert(atomic_load(&completed)==atomic_load(&backend_fence));

    /* Pause drains accepted jobs before GPU drain/reset; resume wakes blocked callers. */
    handles[0]=(NVNcommandHandle)(uintptr_t)&command;arm();
    assert(!pthread_create(&producer,NULL,submit,NULL));await_start();
    pthread_t lifecycle;assert(!pthread_create(&lifecycle,NULL,suspend_gfx,NULL));delay(5000000);assert(!atomic_load(&paused));
    release();assert(!pthread_join(producer,NULL));assert(!pthread_join(lifecycle,NULL));assert(atomic_load(&paused));
    assert(atomic_load(&cache_saves)==1);
    uint64_t tail=dm_nvn_translation_tail(&android_xqueue);
    assert(!pthread_create(&producer,NULL,submit,NULL));delay(5000000);assert(dm_nvn_translation_tail(&android_xqueue)==tail);
    dm_gfx_suspend(0);assert(!pthread_join(producer,NULL));x_drain();assert(!atomic_load(&paused));
    assert(atomic_load(&cache_saves)==1);
    dm_nvn_translation_destroy(&android_xqueue);
    assert(atomic_load(&idle_begins)==atomic_load(&idle_ends));
    assert(synchronous || atomic_load(&idle_begins)>0);
    printf("TRANSLATION_ADAPTER mode=%s handles=owned fences=backend timeout=bounded present=snapshot lifecycle=drained hints=same_thread passed=1\n",synchronous?"sync":"worker");
    return 0;
}
