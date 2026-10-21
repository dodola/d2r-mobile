/* Included by hle_nvn.c only on Android: virtual NVN objects/pacing are retained;
 * no version-94012 game-address hooks or Darwin instrumentation is linked.
 */
#include "nvn_platform_clock.h"
#include "nvn_platform_backend.h"
#include "nvn_present_policy.h"
#include <sys/resource.h>
#include "nvn_translation_queue.h"
#include "nvn_translation_sync.h"
#include "scale3d_android.h"
#include "../gfx/nvn/nvn_vk_memory.h"
static pthread_mutex_t android_suspend_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t android_suspend_cv=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t android_lifecycle_mu=PTHREAD_MUTEX_INITIALIZER;
static int android_suspended,android_inflight;
static _Atomic uint64_t android_nvn_frame;
enum { ANDROID_X_SUBMIT, ANDROID_X_PRESENT, ANDROID_X_BEGIN, ANDROID_X_FENCE };
typedef struct {
    int kind, n;
    const tex_t *texture;
    sync_t *sync;
    double period;
    int crop[4];
    uint64_t *fence_out;   /* present jobs: where to store the backend fence of this present */
    NVNcommandHandle handles[];
} android_xjob;
static dm_nvn_translation_queue android_xqueue;
static pthread_once_t android_xonce=PTHREAD_ONCE_INIT;
static void gw_translate_tick(void);
static void android_xrun(void *payload) {
    android_xjob *j=payload;
    dm_android_nvn_schedule_thread("translate");
    switch(j->kind) {
    case ANDROID_X_SUBMIT:
        dm_android_nvn_work_check_thread();
        mtl_sub_begin();
        mtl_sub_index(j->handles,j->n);
        for(int i=0;i<j->n;i++)mtl_exec((const uint8_t *)(uintptr_t)j->handles[i]);
        mtl_sub_commit_async();
        /* Vulkan retains bound program/target snapshots across submissions.
         * Do not reclaim them using Apple's submission-count snap_tick. */
        break;
    case ANDROID_X_PRESENT: {
        dm_android_nvn_work_check_thread();
        mtl_present(j->texture,j->period,j->crop);
        if(j->fence_out)*j->fence_out=mtl_last_fence();
        gw_translate_tick();
        /* The dynamic resolution controller treats this as the frame time to hit. A 16.7 ms present period would make
         * every device that cannot reach 60 FPS look like it drops every frame and walk down to the lowest scale, so it
         * keeps the 30 FPS target (never shorter than 33.3 ms). */
        {uint64_t target=(uint64_t)(j->period*1e9);if(target<33333332ULL)target=33333332ULL;dm_android_scale3d_present(now_ns(),target);}
        uint64_t frame=atomic_fetch_add(&android_nvn_frame,1)+1;
        mtl_set_frame(frame);dm_android_nvn_presented(frame);
        dm_android_nvn_work_end(frame);break;
    }
    case ANDROID_X_BEGIN: dm_android_nvn_work_begin();break;
    case ANDROID_X_FENCE:
        /* Events, readbacks and presents also allocate Vulkan fence values.
         * Capture the real backend fence here, in translation queue order. */
        dm_nvn_sync_publish(j->sync,mtl_last_fence());break;
    default: die("Android translation job kind %d",j->kind);
    }
    free(j);
}
/* DM_GUEST_WAIT_DIAG: where the guest (game) thread blocks, summed over each 60 presents. */
static struct {_Atomic uint64_t sync_calls,sync_translate_ns,sync_gpu_ns,pace_ns,ticket_ns,push_ns,drain_ns;int enabled;} gw;
static uint64_t gw_now(void){return gw.enabled?now_ns():0;}
/* Per-thread resource usage over each 60 presents (page faults and context switches are not available to simpleperf here). */
static void gw_rusage(const char *who) {
    static _Thread_local struct rusage last;static _Thread_local int have;struct rusage u;
    if(getrusage(RUSAGE_THREAD,&u))return;
    if(have)fprintf(stderr,"[nvn-android] THREAD_RUSAGE who=%s minflt=%ld majflt=%ld vol_csw=%ld invol_csw=%ld user_ms=%.1f sys_ms=%.1f\n",who,u.ru_minflt-last.ru_minflt,u.ru_majflt-last.ru_majflt,
        u.ru_nvcsw-last.ru_nvcsw,u.ru_nivcsw-last.ru_nivcsw,
        ((u.ru_utime.tv_sec-last.ru_utime.tv_sec)*1e3+(u.ru_utime.tv_usec-last.ru_utime.tv_usec)/1e3),
        ((u.ru_stime.tv_sec-last.ru_stime.tv_sec)*1e3+(u.ru_stime.tv_usec-last.ru_stime.tv_usec)/1e3));
    last=u;have=1;
}
/* DM_GUEST_WAIT_DIAG=trace: timeline of the guest thread's NVN calls for a few frames. */
static int gw_trace_on(void){static int on=-1;if(on<0){const char *e=getenv("DM_GUEST_WAIT_DIAG");on=e&&!strcmp(e,"trace");}return on;}
static _Atomic unsigned long long gw_trace_frame;
#define GW_TRACE(...) do{if(gw.enabled&&gw_trace_on()){unsigned long long f=atomic_load(&gw_trace_frame);if(f>=700&&f<706){fprintf(stderr,"[nvn-android] SYNC_TRACE t=%.3f f=%llu ",now_ns()/1e6,f);fprintf(stderr,__VA_ARGS__);}}}while(0)
#define GW_ADD(field,t0) do{if(gw.enabled)atomic_fetch_add(&gw.field,now_ns()-(t0));}while(0)
static void gw_translate_tick(void) {static unsigned n;if(gw.enabled&&++n%60==0)gw_rusage("translate");}
static void android_xidle(int waiting) {
    static _Thread_local uint64_t token;
    if(waiting)token=dm_android_nvn_work_wait_begin(DM_NVN_WAIT_TRANSLATION_QUEUE);
    else dm_android_nvn_work_wait_end(token);
}
static void android_xprepare(void *payload,uint64_t ticket) {
    (void)ticket;
    android_xjob *j=payload;
    if(j->kind==ANDROID_X_FENCE)atomic_fetch_add(&j->sync->pending,1);
    if(j->kind==ANDROID_X_SUBMIT)for(int i=0;i<j->n;i++) {
        if(!dm_nvn_mark_fences((const uint8_t *)(uintptr_t)j->handles[i],0))
            die("Android invalid command stream while indexing syncs handle=%d",i);
    }
}
static void android_xstart(void) {
    gw.enabled=getenv("DM_GUEST_WAIT_DIAG")!=NULL;
    const char *e=getenv("DM_NVN_SYNC");
    int error=dm_nvn_translation_init(&android_xqueue,android_xrun,e&&atoi(e),android_xidle,android_xprepare);
    LOG("[nvn-android] TRANSLATION_QUEUE mode=%s capacity=%d thread_error=%d",
        android_xqueue.synchronous?"synchronous":"worker",DM_NVN_TRANSLATION_CAPACITY,error);
}
static android_xjob *android_xnew(int kind,int n) {
    if(n<0 || (size_t)n>(SIZE_MAX-sizeof(android_xjob))/sizeof(NVNcommandHandle))die("Android translation handle count %d",n);
    android_xjob *j=calloc(1,sizeof(*j)+(size_t)n*sizeof(NVNcommandHandle));
    if(!j)die("Android translation job allocation n=%d",n);
    j->kind=kind;j->n=n;return j;
}
static uint64_t android_xpush(android_xjob *j) {
    pthread_once(&android_xonce,android_xstart);
    uint64_t t0=gw_now();uint64_t ticket=dm_nvn_translation_push(&android_xqueue,j);GW_ADD(push_ns,t0);return ticket;
}
static _Atomic uint64_t gw_line_ns[4096],gw_line_calls[4096];
static void x_drain_at(int line) {
    pthread_once(&android_xonce,android_xstart);
    uint64_t t0=gw_now();dm_nvn_translation_wait(&android_xqueue,dm_nvn_translation_tail(&android_xqueue),UINT64_MAX);GW_ADD(drain_ns,t0);
    if(gw.enabled&&line>=0&&line<4096){atomic_fetch_add(&gw_line_ns[line],now_ns()-t0);atomic_fetch_add(&gw_line_calls[line],1);}
}
static void x_drain(void) {x_drain_at(0);}
/* Command handle -> ticket of the submission that carried it. A handle that was never submitted directly (for
 * example one only called from another command buffer) is not in the table, and then the whole queue is drained
 * exactly as before, so a miss costs time but never correctness. */
#define ANDROID_HT_SIZE 4096
static struct {NVNcommandHandle handle;uint64_t ticket;} android_ht[ANDROID_HT_SIZE];
static pthread_mutex_t android_ht_lock=PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t gw_ht_hit,gw_ht_miss;
static unsigned android_ht_slot(NVNcommandHandle h){return (unsigned)(((uint64_t)h*0x9E3779B97F4A7C15ull)>>52)&(ANDROID_HT_SIZE-1);}
static int android_finalize_mode(void);
static void android_note_submit_handles(const NVNcommandHandle *handles,int n,uint64_t ticket) {
    pthread_mutex_lock(&android_ht_lock);
    for(int i=0;i<n;i++){unsigned s=android_ht_slot(handles[i]);android_ht[s].handle=handles[i];android_ht[s].ticket=ticket;}
    pthread_mutex_unlock(&android_ht_lock);
}
/* DM_FINALIZE_WAIT: tail (default) = drain the whole queue, the original behaviour; ticket = wait for the submission
 * that carried the handle; none = do not wait. The NVN contract keeps command memory valid until the fence of the
 * submission that used it, never until Finalize, so Finalize itself orders nothing. Measured on PPG-AN00: with
 * none/ticket the game thread runs ahead, but replay is then 14% slower (shared page-protection and allocator
 * contention) and the frame rate does not improve, so the original behaviour stays the default. */
static int android_finalize_mode(void) {
    static int mode=-1;
    if(mode<0){const char *e=getenv("DM_FINALIZE_WAIT");mode=!e?0:!strcmp(e,"none")?2:!strcmp(e,"ticket")?1:0;}
    return mode;
}
static void android_finalize_handle(NVNcommandHandle h) {
    int mode=android_finalize_mode();
    if(mode==2)return;
    if(mode==0){x_drain_at(0);return;}
    uint64_t ticket=0;int found=0;
    pthread_mutex_lock(&android_ht_lock);
    unsigned s=android_ht_slot(h);if(android_ht[s].handle==h&&android_ht[s].ticket){ticket=android_ht[s].ticket;found=1;}
    pthread_mutex_unlock(&android_ht_lock);
    pthread_once(&android_xonce,android_xstart);
    if(found){uint64_t t0=gw_now();dm_nvn_translation_wait(&android_xqueue,ticket,UINT64_MAX);GW_ADD(drain_ns,t0);
        if(gw.enabled)atomic_fetch_add(&gw_ht_hit,1);}
    else{x_drain_at(0);if(gw.enabled)atomic_fetch_add(&gw_ht_miss,1);}
}
static void android_queue_fence(sync_t *s) {
    GW_TRACE("fence_queue sync=%p pending=%llu translated=%llu\n"
        ,(void *)s,(unsigned long long)atomic_load(&s->pending),(unsigned long long)atomic_load(&s->translated));
    android_xjob *j=android_xnew(ANDROID_X_FENCE,0);j->sync=s;android_xpush(j);
}
static NVNsyncWaitResult android_sync_wait(const sync_t *s,uint64_t timeout) {
    uint64_t start=now_ns();
    if(gw.enabled)atomic_fetch_add(&gw.sync_calls,1);
    uint64_t generation=atomic_load(&s->pending);
    GW_TRACE("sync_wait sync=%p generation=%llu translated=%llu fenced=%d\n",(void *)s,(unsigned long long)generation,(unsigned long long)atomic_load(&s->translated),(int)s->fenced);
    int ready=atomic_load(&s->translated)>=generation;
    /* Wait only for this fence to be reached, not for the queue tail: a later
     * GPU event may itself need the CPU that is waiting here to signal it. */
    while(atomic_load(&s->translated)<generation) {
        if(timeout!=UINT64_MAX && now_ns()-start>=timeout)return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
        struct timespec pause={0,100000};nanosleep(&pause,NULL);
    }
    GW_ADD(sync_translate_ns,start);
    if(!s->fenced)return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    if(mtl_wait(s->fence,0))return ready?NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED:NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED;
    uint64_t elapsed=now_ns()-start;
    if(timeout!=UINT64_MAX){if(elapsed>=timeout)return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;timeout-=elapsed;}
    uint64_t gpu0=gw_now();int satisfied=mtl_wait(s->fence,timeout);GW_ADD(sync_gpu_ns,gpu0);
    return satisfied?NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED:NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
static void android_gfx_enter(void) {
    pthread_mutex_lock(&android_suspend_mu);
    while(android_suspended)pthread_cond_wait(&android_suspend_cv,&android_suspend_mu);
    android_inflight++;pthread_mutex_unlock(&android_suspend_mu);
}
static void android_gfx_leave(void) {
    pthread_mutex_lock(&android_suspend_mu);android_inflight--;
    pthread_cond_broadcast(&android_suspend_cv);pthread_mutex_unlock(&android_suspend_mu);
}
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&android_lifecycle_mu);
    /* Publish the new workload epoch before waking a resumed acquire caller.
     * Hint service calls never hold android_suspend_mu. */
    if(!on) {void dm_3d_scale_resumed(void);dm_3d_scale_resumed();dm_android_nvn_work_reset(1);}
    pthread_mutex_lock(&android_suspend_mu);android_suspended=on!=0;
    if(on)while(android_inflight)pthread_cond_wait(&android_suspend_cv,&android_suspend_mu);
    else pthread_cond_broadcast(&android_suspend_cv);
    pthread_mutex_unlock(&android_suspend_mu);if(on){
        x_drain();dm_android_nvn_work_reset(0);mtl_drain();dm_vk_pipeline_cache_save();
        DmVkMemoryStats memory;
        int error=dm_vk_memory_trim_caches(DM_VK_TRIM_ALL,&memory);
        if(!error)LOG("ANDROID_GPU_CACHE_TRIM freed=%llu remaining=%llu count=%llu",
            (unsigned long long)memory.last_trim_freed_bytes,
            (unsigned long long)memory.total_bytes,(unsigned long long)memory.trim_count);
        else LOG("ANDROID_GPU_CACHE_TRIM skipped errno=%d",error);
    }
    pthread_mutex_unlock(&android_lifecycle_mu);
}
uint64_t dm_nvn_frame_count(void) {return atomic_load(&android_nvn_frame);}
NVN(nvnQueueSubmitCommands,void,NVNqueue *q,int n,const NVNcommandHandle *handles) {
    if(!q || n<0 || (n&&!handles))die("Android NVN submit parameters n=%d",n);
    android_gfx_enter();
    android_xjob *j=android_xnew(ANDROID_X_SUBMIT,n);
    if(n)memcpy(j->handles,handles,(size_t)n*sizeof(*handles));
    ((queue_t *)q)->submits++;
    GW_TRACE("submit n=%d\n",n);
    uint64_t ticket=android_xpush(j);
    if(n&&android_finalize_mode()==1)android_note_submit_handles(handles,n,ticket);
    android_gfx_leave();
}
__attribute__((constructor)) static void android_nvn_register(void) {
    nvn_init_slots();
    unsigned profiled=0;
    if(getenv("DM_NVN_PROF"))for(unsigned i=0;i<NVN_NFUNC;i++) {
        if(nvn_slot[i].fn!=(void *)nvn_unimpl_entry && strcmp(nvn_names[i],"nvnQueuePresentTexture")) {
            nvn_real[i]=nvn_slot[i].fn;nvn_slot[i].fn=(void *)nvn_prof_entry;profiled++;
        }
    }
    LOG("[nvn-android] ELF interfaces=%u implemented=%u named-stop=%u profiled=%u; zero unknown-success stubs",
        NVN_NFUNC,g_nsem,NVN_NFUNC-g_nsem,profiled);
}
#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults,void,NVNwindowBuilder *b) {NVNdevice *d=WB(b)->dev;memset(b,0,sizeof(win_builder_t));WB(b)->dev=d;WB(b)->interval=1;}
NVN(nvnWindowBuilderSetDevice,void,NVNwindowBuilder *b,NVNdevice *d) {WB(b)->dev=d;}
NVN(nvnWindowBuilderSetNativeWindow,void,NVNwindowBuilder *b,NVNnativeWindow native) {WB(b)->native=native;}
NVN(nvnWindowBuilderSetTextures,void,NVNwindowBuilder *b,int n,NVNtexture *const *textures) {
    if(n<1 || n>4 || !textures)die("Android NVN window texture count %d",n);
    WB(b)->ntex=n;
    for(int i=0;i<n;i++){if(!textures[i])die("Android NVN null window texture %d",i);WB(b)->tex[i]=textures[i];}
}
NVN(nvnWindowBuilderSetPresentInterval,void,NVNwindowBuilder *b,int interval) {if(interval<0)die("negative present interval %d",interval);WB(b)->interval=interval;}
static void android_present_log(const win_t *w);
NVN(nvnWindowInitialize,NVNboolean,NVNwindow *window,const NVNwindowBuilder *b) {
    const win_builder_t *s=(const void *)b;if(s->ntex<1 || s->ntex>4)return 0;
    void *native=dm_vk_native_window();
    if(!native || (void *)(uintptr_t)s->native!=native)die("Android NVN window requires currently bound real Surface");
    win_t *w=(void *)window;memset(w,0,sizeof(*w));w->magic=MAGIC('W','I','N','D');w->native=s->native;
    w->ntex=s->ntex;w->interval=s->interval;memcpy(w->tex,s->tex,sizeof(w->tex));android_present_log(w);return 1;
}
NVN(nvnWindowFinalize,void,NVNwindow *window) {x_drain();mtl_finish();memset(window,0,sizeof(win_t));}
NVN(nvnWindowSetPresentInterval,void,NVNwindow *window,int interval) {
    if(interval<0)die("negative present interval %d",interval);
    win_t *w=(void *)window;
    if(w->interval!=interval){w->interval=interval;android_present_log(w);}
}
NVN(nvnWindowGetPresentInterval,int,const NVNwindow *window) {return ((const win_t *)window)->interval;}
NVN(nvnWindowSetCrop,void,NVNwindow *window,int x,int y,int width,int height) {
    if(x<0 || y<0 || width<0 || height<0)die("NVN crop bounds %d %d %d %d",x,y,width,height);
    int crop[4]={x,y,width,height};memcpy(((win_t *)window)->crop,crop,sizeof(crop));
}
static int android_present_hz=-1,android_display_intervals,android_present_fixed_60=-1;
/* The graphics OBJECT library is shared by generic titles and D2R. Only the
 * D2R target defines this policy callback; target defines do not propagate
 * into the independently compiled graphics objects. */
extern int dm_android_d2r_fps_setting(void) __attribute__((weak));
void nvn_present_config(int hz,int display_intervals) {if(hz<0 || hz>1000)die("NVN pacing hz=%d",hz);android_present_hz=hz;android_display_intervals=display_intervals;}
static uint64_t android_present_period(const win_t *w) {
    if(android_present_hz<0) {const char *e=getenv("DM_PRESENT_HZ");
        android_present_hz=e?atoi(e):dm_android_cadence_base_hz(mtl_display_hz(),getenv("DM_DISABLE_PRESENT_CADENCE")!=NULL);
        android_display_intervals=mtl_has_window();}
    /* The game's power-saving option is 30 FPS when on and 60 FPS when off, and the upstream iOS build presents at the
     * display rate when it is off. Do the same here; diagnostics/disable-present-60fps restores the old behaviour of
     * following the game's own present interval (30 FPS). */
    /* 默认按 16.7 ms 出图；显式的 DM_PRESENT_HZ（含 0 = 不限速的诊断出口）或 disable-present-60fps 恢复游戏自己的出图间隔。
     * Present every 16.7 ms by default; an explicit DM_PRESENT_HZ (including 0 = the uncapped diagnostic escape) or
     * disable-present-60fps restores the game's own present interval. */
    if(android_present_fixed_60<0)android_present_fixed_60=getenv("DM_ANDROID_PRESENT_60FPS")!=NULL||(!getenv("DM_DISABLE_PRESENT_60FPS")&&!getenv("DM_PRESENT_HZ"));
    uint64_t period=dm_android_present_period_ns(android_present_hz,android_display_intervals,w->interval,android_present_fixed_60);
    /* Read applied settings on every frame. Apply/Discard keep their shared
     * semantics; the explicit fixed60 diagnostic retains precedence. */
    if(dm_android_d2r_fps_setting)
        period=dm_android_present_power_period_ns(period,dm_android_d2r_fps_setting()==30);
    return period;
}
static void android_present_log(const win_t *w) {
    uint64_t period=android_present_period(w);
    LOG("[nvn-android] PRESENT_POLICY mode=%s base_hz=%d guest_interval=%d apply_guest_interval=%d effective_period_ns=%llu",
        android_present_fixed_60?"fixed60":"guest",android_present_hz,w->interval,
        android_display_intervals&&!android_present_fixed_60,(unsigned long long)period);
}
NVN(nvnWindowAcquireTexture,NVNwindowAcquireTextureResult,NVNwindow *window,NVNsync *sync,int *index) {
    win_t *w=(void *)window;if(!index || w->ntex<1 || w->ntex>4)die("NVN acquire window",0);
    GW_TRACE("acquire\n");
    android_gfx_enter();dm_android_nvn_schedule_thread("acquire");*index=w->next++%w->ntex;
    uint64_t now=now_ns(),period=android_present_period(w);nvn_pacer p={w->pace};uint64_t wait=nvn_pace(&p,now,period);w->pace=p.target;
    if(wait){uint64_t t0=gw_now();dm_android_nvn_sleep_until(now+wait);GW_ADD(pace_ns,t0);}
    /* These are translation tickets, not Vulkan fence/present numbers. The
     * backend owns image/GPU reuse; its present_wait only checks ticket validity. */
    if(w->ticket[*index]){uint64_t t0=gw_now();dm_nvn_translation_wait(&android_xqueue,w->ticket[*index],UINT64_MAX);GW_ADD(ticket_ns,t0);}
    if(sync) {
        /* NVN: the acquire sync means "this image is free", i.e. its previous present is done. Queuing a FENCE job
         * (the default) publishes the newest backend fence behind ALL earlier jobs. DM_ACQUIRE_SYNC=early publishes
         * the fence of this image's previous present instead; it did not change the frame rate by itself. */
        static int early=-1;
        if(early<0){const char *e=getenv("DM_ACQUIRE_SYNC");early=e&&!strcmp(e,"early");}
        if(early)dm_nvn_sync_publish((sync_t *)sync,w->pfence[*index]);
        else android_queue_fence((sync_t *)sync);
    }
    android_xpush(android_xnew(ANDROID_X_BEGIN,0));android_gfx_leave();return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
int (*g_nvn_loadscreen_up)(void);
NVN(nvnQueuePresentTexture,void,NVNqueue *q,NVNwindow *window,int index) {
    (void)q;win_t *w=(void *)window;if(index<0 || index>=w->ntex || index>=4 || !w->tex[index])die("NVN present index %d",index);
    GW_TRACE("present index=%d\n",index);atomic_fetch_add(&gw_trace_frame,1);
    android_gfx_enter();android_xjob *j=android_xnew(ANDROID_X_PRESENT,0);
    j->fence_out=&w->pfence[index];
    j->texture=snap_tex(w->tex[index]);j->period=android_present_period(w)/1e9;memcpy(j->crop,w->crop,sizeof(j->crop));
    w->ticket[index]=android_xpush(j);w->presents++;
    if(gw.enabled&&w->presents%60==0){
        fprintf(stderr,"[nvn-android] GUEST_WAIT presents=%llu sync_calls=%llu sync_translate_ms=%.1f sync_gpu_ms=%.1f pace_ms=%.1f acquire_ticket_ms=%.1f push_ms=%.1f drain_ms=%.1f%s",
            (unsigned long long)w->presents,(unsigned long long)atomic_exchange(&gw.sync_calls,0),atomic_exchange(&gw.sync_translate_ns,0)/1e6,
            atomic_exchange(&gw.sync_gpu_ns,0)/1e6,atomic_exchange(&gw.pace_ns,0)/1e6,atomic_exchange(&gw.ticket_ns,0)/1e6,
            atomic_exchange(&gw.push_ns,0)/1e6,atomic_exchange(&gw.drain_ns,0)/1e6,"\n");
        fprintf(stderr,"[nvn-android] FINALIZE_HANDLE hit=%llu miss=%llu""\n",(unsigned long long)atomic_exchange(&gw_ht_hit,0),(unsigned long long)atomic_exchange(&gw_ht_miss,0));
        gw_rusage("guest");
        {static uint64_t pf,pp,pc,pg,pn;uint64_t faults,preps,calls,pages,ns;
         nvn_dirty_stats(&faults,&preps);nvn_dirty_mp_stats(&calls,&pages,&ns);
         fprintf(stderr,"[nvn-android] DIRTY_COST faults=%llu host_preps=%llu mprotect_calls=%llu mprotect_pages=%llu mprotect_ms=%.1f\n",
            (unsigned long long)(faults-pf),(unsigned long long)(preps-pp),(unsigned long long)(calls-pc),(unsigned long long)(pages-pg),(ns-pn)/1e6);
         pf=faults;pp=preps;pc=calls;pg=pages;pn=ns;}
        {static uint64_t prev[NVN_NFUNC];uint64_t delta[NVN_NFUNC];
         for(unsigned i=0;i<NVN_NFUNC;i++){uint64_t c=atomic_load(&nvn_slot[i].cnt);delta[i]=c-prev[i];prev[i]=c;}
         for(int top=0;top<80;top++){unsigned best=0;for(unsigned i=1;i<NVN_NFUNC;i++)if(delta[i]>delta[best])best=i;
            if(!delta[best])break;
            fprintf(stderr,"[nvn-android] NVN_CALLS %s per_frame=%.1f\n",nvn_names[best],(double)delta[best]/60.0);delta[best]=0;}}
        for(int top=0;top<4;top++){int best=-1;uint64_t bn=0;
            for(int l=0;l<4096;l++){uint64_t n=atomic_load(&gw_line_ns[l]);if(n>bn){bn=n;best=l;}}
            if(best<0)break;
            fprintf(stderr,"[nvn-android] GUEST_DRAIN_SITE line=%d calls=%llu ms=%.1f""\n",best,(unsigned long long)atomic_exchange(&gw_line_calls[best],0),atomic_exchange(&gw_line_ns[best],0)/1e6);}
        for(int l=0;l<4096;l++){atomic_store(&gw_line_ns[l],0);atomic_store(&gw_line_calls[l],0);}}
    android_gfx_leave();
}
