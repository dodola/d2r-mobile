/* Included by hle_nvn.c only on Android: virtual NVN objects/pacing are retained;
 * no version-94012 game-address hooks or Darwin instrumentation is linked.
 */
#include "nvn_platform_clock.h"
#include "nvn_platform_backend.h"
#include "nvn_present_policy.h"
#include "scale3d_android.h"
#include "../gfx/nvn/nvn_vk_memory.h"
static pthread_mutex_t android_suspend_mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t android_suspend_cv=PTHREAD_COND_INITIALIZER;
static pthread_mutex_t android_lifecycle_mu=PTHREAD_MUTEX_INITIALIZER;
static int android_suspended,android_inflight;
static _Atomic uint64_t android_nvn_frame;
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
        dm_android_nvn_work_reset(0);mtl_drain();
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
    android_gfx_enter();dm_android_nvn_schedule_thread("submit");dm_android_nvn_work_check_thread();
    ((queue_t *)q)->submits++;mtl_sub_begin();
    for(int i=0;i<n;i++)mtl_exec((const uint8_t *)(uintptr_t)handles[i]);
    mtl_sub_commit_async();android_gfx_leave();
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
    if(n<1 || n>4 || !textures)die("Android NVN window texture count %d",n);WB(b)->ntex=n;
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
NVN(nvnWindowFinalize,void,NVNwindow *window) {mtl_finish();memset(window,0,sizeof(win_t));}
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
    if(android_present_hz<0) {const char *e=getenv("DM_PRESENT_HZ");android_present_hz=e?atoi(e):60;
        android_display_intervals=mtl_has_window();}
    if(android_present_fixed_60<0)android_present_fixed_60=getenv("DM_ANDROID_PRESENT_60FPS")!=NULL;
    uint64_t period=dm_android_present_period_ns(android_present_hz,android_display_intervals,w->interval,android_present_fixed_60);
    /* Read applied settings on every frame. Apply/Discard keep their shared
     * semantics; the explicit fixed60 diagnostic retains precedence. */
    if(!android_present_fixed_60 && dm_android_d2r_fps_setting)
        period=dm_android_present_power_period_ns(period,dm_android_d2r_fps_setting()==30);
    return period;
}
static void android_present_log(const win_t *w) {
    uint64_t period=android_present_period(w);
    LOG("[nvn-android] PRESENT_POLICY mode=%s base_hz=%d guest_interval=%d apply_guest_interval=%d effective_period_ns=%llu",
        android_present_fixed_60?"fixed60-experiment":"guest",android_present_hz,w->interval,
        android_display_intervals&&!android_present_fixed_60,(unsigned long long)period);
}
NVN(nvnWindowAcquireTexture,NVNwindowAcquireTextureResult,NVNwindow *window,NVNsync *sync,int *index) {
    win_t *w=(void *)window;if(!index || w->ntex<1 || w->ntex>4)die("NVN acquire window",0);
    android_gfx_enter();dm_android_nvn_schedule_thread("acquire");dm_android_nvn_work_check_thread();*index=w->next++%w->ntex;
    uint64_t now=now_ns(),period=android_present_period(w);nvn_pacer p={w->pace};uint64_t wait=nvn_pace(&p,now,period);w->pace=p.target;
    if(wait)dm_android_nvn_sleep_until(now+wait);
    if(w->ticket[*index])mtl_present_wait(w->ticket[*index]);
    if(sync){sync_t *s=(void *)sync;s->fenced=1;s->fence=mtl_last_fence();}
    dm_android_nvn_work_begin();android_gfx_leave();return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
int (*g_nvn_loadscreen_up)(void);
NVN(nvnQueuePresentTexture,void,NVNqueue *q,NVNwindow *window,int index) {
    (void)q;win_t *w=(void *)window;if(index<0 || index>=w->ntex || index>=4 || !w->tex[index])die("NVN present index %d",index);
    android_gfx_enter();dm_android_nvn_schedule_thread("present");dm_android_nvn_work_check_thread();
    uint64_t period=android_present_period(w);
    w->ticket[index]=mtl_present((const tex_t *)w->tex[index],period/1e9,w->crop);w->presents++;
    /* Real FIFO acquisition / queue-present return cadence, not an assumed
     * FPS or a physical scanout-completion timestamp. */
    dm_android_scale3d_present(now_ns(),period);
    uint64_t frame=atomic_fetch_add(&android_nvn_frame,1)+1;mtl_set_frame(frame);dm_android_nvn_presented(frame);
    dm_android_nvn_work_end(frame);android_gfx_leave();
}
