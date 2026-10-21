/* Included by hle_oe.c after the shared Apple/Android scale3d controller. */
#include "scale3d_android.h"
#include "render_scale.h"
#include "render_budget.h"
#include "d2r_scale3d_91738_profile.h"
#include "d2r_scale3d_profile.h"
#include <sys/uio.h>
#include <unistd.h>

extern uint64_t dm_nvn_frame_count(void) __attribute__((weak));
extern int dm_nvn_ruler_truth(void) __attribute__((weak));
extern int dm_android_thermal_status_now(void) __attribute__((weak));
static pthread_mutex_t android_s3_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t android_s3_once=PTHREAD_ONCE_INIT;
static scale3d_ctl android_s3_ctl;
static dm_android_scale3d_status android_s3={.configured=.75f,.current=1.f,.floor=S3_FLOOR_DEF,.thermal_status=-1};
static int android_s3_dynamic=1,android_s3_in_game,android_s3_have_owner;
static pthread_t android_s3_owner;
static unsigned android_s3_quality_epoch;
static _Atomic float android_s3_current=1.f;
static _Thread_local int android_s3_rebuilding;
/* UI requests publish host state only. A saved/manual choice is fixed on
 * either validated version and takes precedence over the dynamic controller. */
static int android_s3_manual,android_s3_build;
static float android_s3_requested=1.f,android_s3_91738_configured=1.f;
int dm_android_render_scale_request(float scale) {
    if(!isfinite(scale) || scale<.70f || scale>1.f)return EINVAL;
    pthread_mutex_lock(&android_s3_lock);
    android_s3_manual=1;android_s3_requested=scale;
    android_s3_ctl.level=0;android_s3.level=0;atomic_store(&g_scale3d_level,0);
    atomic_store(&g_scale3d_resumed,1);
    pthread_mutex_unlock(&android_s3_lock);return 0;
}
int dm_android_render_scale_auto(void) {
    pthread_mutex_lock(&android_s3_lock);
    android_s3_manual=0;android_s3_requested=1.f;
    android_s3_ctl.level=0;android_s3.level=0;
    atomic_store(&g_scale3d_level,0);
    atomic_store(&g_scale3d_resumed,1);
    pthread_mutex_unlock(&android_s3_lock);return 0;
}
float dm_android_render_scale_requested(void) {
    pthread_mutex_lock(&android_s3_lock);float scale=android_s3_manual?android_s3_requested:0.f;
    pthread_mutex_unlock(&android_s3_lock);return scale;
}

static float android_s3_env(const char *key,float fallback) {
    const char *e=getenv(key);if(!e)return fallback;
    char *end;errno=0;float value=strtof(e,&end);
    if(end==e || *end || errno==ERANGE || !isfinite(value) || value<.5f || value>1.f)
        die("ANDROID_3D_SCALE invalid %s=%s; requires finite 0.5..1.0",key,e);
    return value;
}
static void android_s3_configure(void) {
    float configured=android_s3_env("DM_3D_SCALE",.75f);
    float legacy=android_s3_env("DM_3D_SCALE",1.f),floor=android_s3_env("DM_3D_SCALE_FLOOR",S3_FLOOR_DEF);
    int max_level=0;for(int i=0;i<S3_NLV;i++)if(g_s3_levels[i]>=floor-1e-4f)max_level=i+1;
    const char *e=getenv("DM_3D_SCALE_DYN");
    int dynamic=!(e && !strcmp(e,"0")) && !getenv("DM_THERMAL_NOFALLBACK");
    pthread_mutex_lock(&android_s3_lock);
    android_s3.configured=configured;android_s3_91738_configured=legacy;android_s3.floor=floor;
    android_s3_ctl.max_level=max_level;android_s3.max_level=max_level;android_s3_dynamic=dynamic;
    pthread_mutex_unlock(&android_s3_lock);
    LOG("ANDROID_3D_SCALE_CONFIG profiles=91738-fixed,94012-dynamic base=%.3f floor=%.3f max_level=%d dynamic=%d manual_request_overrides=1 source=queue-present-return scanout_completion=0 UI_output_unchanged=1",
        (double)configured,(double)floor,max_level,dynamic);
}
static int android_s3_read(uintptr_t p,void *out,size_t bytes) {
    if(!p || !bytes || p>UINTPTR_MAX-bytes)return 0;
    struct iovec local={out,bytes},remote={(void *)p,bytes};
    return process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)bytes;
}
static int android_s3_identity(const module_t *m) {
    return m && m->base && !strcmp(m->name,"main") &&
        !strcmp(m->build_id,"db0507f36b84200ac05f75d76ed5026f") &&
        m->text_end==0x1b786a0 && m->ro_off==0x1b79000 && m->data_off==0x2056000 && m->size>=0x3214000;
}
static int android_s3_profile(const module_t *m) {
    if(!android_s3_identity(m))return 0;
    uint32_t words[32];
    for(size_t i=0;i<sizeof dm_s3_anchors/sizeof *dm_s3_anchors;i++)
        if(!android_s3_read((uintptr_t)m->base+dm_s3_anchors[i].address,words,dm_s3_anchors[i].count*4) ||
           memcmp(words,dm_s3_anchors[i].words,dm_s3_anchors[i].count*4))return 0;
    return 1;
}
static void android_s3_feed_locked(double interval,uint64_t now,uint64_t period,int in_game) {
    scale3d_ctl *c=&android_s3_ctl;
    unsigned quality_epoch=atomic_load(&g_scale3d_quality_epoch);
    android_s3.quality=atomic_load(&g_3d_quality_mode);
    if(quality_epoch!=android_s3_quality_epoch) {
        android_s3_quality_epoch=quality_epoch;c->level=0;scale3d_ctl_hold(c,now,3*S3_SEC);
    }
    if(atomic_exchange(&g_scale3d_resumed,0))scale3d_ctl_hold(c,now,3*S3_SEC);
    if(period!=android_s3.target_period_ns) {
        android_s3.target_period_ns=period;c->period_ms=(double)period/1e6;
        scale3d_ctl_hold(c,now,2*S3_SEC);
    }
    if(!in_game) {android_s3_in_game=0;scale3d_ctl_hold(c,now,S3_SEC);}
    else if(!android_s3_in_game) {android_s3_in_game=1;scale3d_ctl_hold(c,now,20*S3_SEC);}
    android_s3.samples++;android_s3.interval_ms=isfinite(interval)?interval:0;
    if(android_s3_build!=94012 || android_s3_manual || !android_s3.supported || android_s3.disabled ||
       !android_s3_dynamic || android_s3.quality || !period) {
        c->level=0;scale3d_ctl_hold(c,now,S3_SEC);
    } else if(in_game && isfinite(interval)) {
        int before=c->level;scale3d_ctl_step(c,interval,now);
        if(before!=c->level)LOG("ANDROID_3D_SCALE_LEVEL old=%d new=%d target_ms=%.3f drop10=%.4f drop60=%.4f thermal=%d hold_until=%llu",
            before,c->level,c->period_ms,c->last_r10,c->last_r60,android_s3.thermal_status,(unsigned long long)c->hold_until);
    }
    atomic_store(&g_scale3d_level,c->level);android_s3.level=c->level;
    android_s3.hold_until=c->hold_until;android_s3.drop_rate_10s=c->last_r10;android_s3.drop_rate_60s=c->last_r60;
}
void dm_android_scale3d_thermal(int status) {
    if(status< -1 || status>6)return;
    pthread_mutex_lock(&android_s3_lock);
    if(android_s3.thermal_status!=status)LOG("ANDROID_3D_SCALE_THERMAL old=%d new=%d policy=observe-present-drops-only",android_s3.thermal_status,status);
    android_s3.thermal_status=status;
    pthread_mutex_unlock(&android_s3_lock);
    dm_android_platform_setting("thermal_warning",status>=3);
}
void dm_android_scale3d_present(uint64_t presented_ns,uint64_t target_period_ns) {
    pthread_once(&android_s3_once,android_s3_configure);
    int in_game=dm_ingame_now(),poll_thermal=0;
    pthread_mutex_lock(&android_s3_lock);
    uint64_t last=android_s3.last_present_ns;
    double interval=last && presented_ns>last ? (double)(presented_ns-last)/1e6 : 0;
    if(last && presented_ns<=last)scale3d_ctl_hold(&android_s3_ctl,presented_ns,3*S3_SEC);
    static uint64_t last_thermal_poll;
    if(!last_thermal_poll || presented_ns<last_thermal_poll || presented_ns-last_thermal_poll>=S3_SEC) {
        last_thermal_poll=presented_ns;poll_thermal=1;
    }
    android_s3.last_present_ns=presented_ns;
    android_s3_feed_locked(interval,presented_ns,target_period_ns,in_game);
    pthread_mutex_unlock(&android_s3_lock);
    if(poll_thermal && dm_android_thermal_status_now)dm_android_scale3d_thermal(dm_android_thermal_status_now());
}
void dm_3d_scale_feed(double interval_ms) {
    pthread_once(&android_s3_once,android_s3_configure);
    int in_game=dm_ingame_now();uint64_t now=now_ns();
    pthread_mutex_lock(&android_s3_lock);
    android_s3_feed_locked(interval_ms,now,android_s3.target_period_ns,in_game);
    pthread_mutex_unlock(&android_s3_lock);
}
void dm_3d_scale_hot(int hot) {dm_android_platform_setting("thermal_warning",hot!=0);}
float dm_3d_scale_cur(void) {return atomic_load(&android_s3_current);}
void dm_android_scale3d_status_get(dm_android_scale3d_status *out) {
    if(!out)return;
    pthread_mutex_lock(&android_s3_lock);*out=android_s3;
    if(android_s3_manual)out->configured=android_s3_requested;
    else if(android_s3_build==91738)out->configured=android_s3_91738_configured;
    pthread_mutex_unlock(&android_s3_lock);
}
static void android_scale3d_tick_94012(void *return_address) {
    if(android_s3_rebuilding || !dm_nvn_frame_count || dm_nvn_frame_count()<30)return;
    pthread_once(&android_s3_once,android_s3_configure);
    module_t *main=NULL;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){main=&g_mod[i];break;}
    if(!android_s3_identity(main)) {
        pthread_mutex_lock(&android_s3_lock);android_s3.supported=0;pthread_mutex_unlock(&android_s3_lock);return;
    }
    uintptr_t caller=(uintptr_t)return_address-(uintptr_t)main->base;
    if(caller!=0xe10d58 && caller!=0xe10db0)return;
    pthread_mutex_lock(&android_s3_lock);
    if(android_s3.disabled || (android_s3_have_owner && !pthread_equal(android_s3_owner,pthread_self()))) {
        pthread_mutex_unlock(&android_s3_lock);return;
    }
    pthread_mutex_unlock(&android_s3_lock);
    if(!android_s3_profile(main)) {
        pthread_mutex_lock(&android_s3_lock);android_s3.supported=0;pthread_mutex_unlock(&android_s3_lock);return;
    }
    uintptr_t renderer=0;uint32_t sizes[6];float before;
    if(!android_s3_read((uintptr_t)main->base+0x31e0848,&renderer,8) || !renderer || (renderer&15) || renderer>UINTPTR_MAX-0xee90 ||
       !android_s3_read(renderer+0xee78,sizes,sizeof sizes) || !android_s3_read(renderer+0x66a4,&before,4) ||
       !isfinite(before) || before<.5f || before>1.f || !sizes[4] || !sizes[5] || sizes[4]>8192 || sizes[5]>8192)return;
    pthread_mutex_lock(&android_s3_lock);
    if(android_s3_have_owner && !pthread_equal(android_s3_owner,pthread_self())) {pthread_mutex_unlock(&android_s3_lock);return;}
    if(!android_s3_have_owner) {android_s3_owner=pthread_self();android_s3_have_owner=1;}
    android_s3_build=94012;android_s3.supported=1;android_s3.last_caller=caller;
    android_s3.quality=atomic_load(&g_3d_quality_mode);
    int level=atomic_load(&g_scale3d_level);
    float want=android_s3_manual?android_s3_requested:android_s3.configured;
    const char *tier=getenv("DM_RESOLUTION_TIER");
    if(!android_s3_manual&&!getenv("DM_3D_SCALE")&&tier&&tier[0]>='0'&&tier[0]<='2'&&!tier[1])
        want=dm_android_world_scale(sizes[4],sizes[5],tier[0]-'0');
    if(!android_s3_manual && !android_s3.quality && level>0 && g_s3_levels[(level>S3_NLV?S3_NLV:level)-1]<want)
        want=g_s3_levels[(level>S3_NLV?S3_NLV:level)-1];
    if(dm_nvn_ruler_truth && dm_nvn_ruler_truth())want=1.f;
    android_s3.current=before;android_s3.internal_width=sizes[0];android_s3.internal_height=sizes[1];
    android_s3.output_width=sizes[4];android_s3.output_height=sizes[5];
    pthread_mutex_unlock(&android_s3_lock);
    atomic_store(&android_s3_current,before);
    if(want==before)return;
    uint32_t expected_w=(uint32_t)lroundf((float)sizes[4]*want),expected_h=(uint32_t)lroundf((float)sizes[5]*want);
    LOG("ANDROID_3D_SCALE_BEGIN build=94012 before=%.3f want=%.3f internal=%ux%u output=%ux%u caller=%#zx frame=%llu",
        (double)before,(double)want,sizes[0],sizes[1],sizes[4],sizes[5],(size_t)caller,(unsigned long long)dm_nvn_frame_count());
    android_s3_rebuilding=1;uint64_t start=now_ns();
    dm_guest_write_prepare((void *)(renderer+0x66a4),4);
    struct iovec local={&want,4},remote={(void *)(renderer+0x66a4),4};
    if(process_vm_writev(getpid(),&local,1,&remote,1,0)!=4)die("ANDROID_3D_SCALE renderer scale write failed errno=%d",errno);
    ((void (*)(uint32_t,uint32_t))(main->base+0x8582f0))(sizes[4],sizes[5]);
    uint32_t after[6];uintptr_t current_renderer=0;float actual=0;
    int valid=android_s3_read((uintptr_t)main->base+0x31e0848,&current_renderer,8) && current_renderer==renderer &&
        android_s3_read(renderer+0xee78,after,sizeof after) && android_s3_read(renderer+0x66a4,&actual,4) && actual==want &&
        after[0]==expected_w && after[1]==expected_h && after[2]==expected_w && after[3]==expected_h && after[4]==sizes[4] && after[5]==sizes[5];
    if(!valid)die("ANDROID_3D_SCALE rebuild postcondition failed build=94012 expected3d=%ux%u expected_output=%ux%u",expected_w,expected_h,sizes[4],sizes[5]);
    atomic_store(&android_s3_current,want);
    pthread_mutex_lock(&android_s3_lock);
    android_s3.current=want;android_s3.internal_width=after[0];android_s3.internal_height=after[1];android_s3.changes++;
    pthread_mutex_unlock(&android_s3_lock);android_s3_rebuilding=0;
    dm_3d_scale_resumed(); /* Do not count resource-rebuild time as GPU pressure. */
    LOG("ANDROID_3D_SCALE_END build=94012 scale=%.3f internal=%ux%u duplicate=%ux%u output=%ux%u UI_output_unchanged=1 rebuild_ms=%.3f frame=%llu",
        (double)want,after[0],after[1],after[2],after[3],after[4],after[5],(double)(now_ns()-start)/1e6,(unsigned long long)dm_nvn_frame_count());
}

/* main's legacy artifacts predate BUILD_ID metadata. Preserve their exact
 * layout + all twelve instruction guards; a nonempty ID must also match.
 * This legacy route never weakens the mandatory 94012 BuildID guard. */
static int android_s3_identity_91738(const module_t *m) {
    return m && m->base && !strcmp(m->name,"main") &&
        (!m->build_id[0] || !strcmp(m->build_id,"2c7f979610e9911979982dc07bfc5702")) &&
        m->text_end==0x1b4f5d0 && m->ro_off==0x1b50000 && m->data_off==0x2028000 && m->size>=0x31b2538;
}
static int android_s3_profile_91738(const module_t *m) {
    if(!android_s3_identity_91738(m))return 0;
    uint32_t words[30];
    for(size_t i=0;i<sizeof dm_s3_91738_anchors/sizeof *dm_s3_91738_anchors;i++)
        if(!android_s3_read((uintptr_t)m->base+dm_s3_91738_anchors[i].address,words,dm_s3_91738_anchors[i].count*4) ||
           memcmp(words,dm_s3_91738_anchors[i].words,dm_s3_91738_anchors[i].count*4))return 0;
    return 1;
}
static void android_scale3d_tick_91738(module_t *main,void *return_address) {
    uintptr_t caller=(uintptr_t)return_address-(uintptr_t)main->base;
    if(caller!=0xdf7bb8 && caller!=0xdf7c10)return;
    pthread_mutex_lock(&android_s3_lock);
    int blocked=android_s3.disabled || (android_s3_have_owner && !pthread_equal(android_s3_owner,pthread_self()));
    pthread_mutex_unlock(&android_s3_lock);if(blocked)return;
    if(!android_s3_profile_91738(main)) {
        pthread_mutex_lock(&android_s3_lock);android_s3.supported=0;pthread_mutex_unlock(&android_s3_lock);return;
    }
    uintptr_t renderer=0;uint32_t sizes[6];float before;
    if(!android_s3_read((uintptr_t)main->base+0x31b2528,&renderer,8) || !renderer || (renderer&15) || renderer>UINTPTR_MAX-0xee90 ||
       !android_s3_read(renderer+0xee78,sizes,sizeof sizes) || !android_s3_read(renderer+0x66a4,&before,4) ||
       !isfinite(before) || before<.70f || before>1.f || sizes[4]!=1280 || sizes[5]!=720)return;
    pthread_mutex_lock(&android_s3_lock);
    if(android_s3_have_owner && !pthread_equal(android_s3_owner,pthread_self())) {pthread_mutex_unlock(&android_s3_lock);return;}
    float want=android_s3_manual?android_s3_requested:android_s3_91738_configured;
    /* A .50-.69 dynamic environment value is not an approved legacy mode. */
    if(want<.70f || want>1.f) {android_s3.supported=0;pthread_mutex_unlock(&android_s3_lock);return;}
    if(!android_s3_have_owner) {android_s3_owner=pthread_self();android_s3_have_owner=1;}
    android_s3_build=91738;android_s3.supported=1;android_s3.last_caller=caller;
    android_s3.current=before;android_s3.internal_width=sizes[0];android_s3.internal_height=sizes[1];
    android_s3.output_width=sizes[4];android_s3.output_height=sizes[5];
    pthread_mutex_unlock(&android_s3_lock);atomic_store(&android_s3_current,before);
    if(want==before)return;
    uint32_t expected_w=(uint32_t)lroundf((float)sizes[4]*want),expected_h=(uint32_t)lroundf((float)sizes[5]*want);
    LOG("ANDROID_3D_SCALE_BEGIN build=91738 before=%.3f want=%.3f internal=%ux%u output=%ux%u caller=%#zx frame=%llu",
        (double)before,(double)want,sizes[0],sizes[1],sizes[4],sizes[5],(size_t)caller,(unsigned long long)dm_nvn_frame_count());
    android_s3_rebuilding=1;uint64_t start=now_ns();
    dm_guest_write_prepare((void *)(renderer+0x66a4),4);
    struct iovec local={&want,4},remote={(void *)(renderer+0x66a4),4};
    if(process_vm_writev(getpid(),&local,1,&remote,1,0)!=4)die("ANDROID_3D_SCALE renderer scale write failed errno=%d",errno);
    ((void (*)(uint32_t,uint32_t))(main->base+0x841540))(sizes[4],sizes[5]);
    uint32_t after[6];uintptr_t current_renderer=0;float actual=0;
    int valid=android_s3_read((uintptr_t)main->base+0x31b2528,&current_renderer,8) && current_renderer==renderer &&
        android_s3_read(renderer+0xee78,after,sizeof after) && android_s3_read(renderer+0x66a4,&actual,4) && actual==want &&
        after[0]==expected_w && after[1]==expected_h && after[2]==expected_w && after[3]==expected_h && after[4]==sizes[4] && after[5]==sizes[5];
    if(!valid)die("ANDROID_3D_SCALE rebuild postcondition failed build=91738 expected3d=%ux%u expected_output=%ux%u",expected_w,expected_h,sizes[4],sizes[5]);
    atomic_store(&android_s3_current,want);
    pthread_mutex_lock(&android_s3_lock);
    android_s3.current=want;android_s3.internal_width=after[0];android_s3.internal_height=after[1];android_s3.changes++;
    pthread_mutex_unlock(&android_s3_lock);android_s3_rebuilding=0;dm_3d_scale_resumed();
    LOG("ANDROID_3D_SCALE_END build=91738 scale=%.3f internal=%ux%u duplicate=%ux%u output=%ux%u UI_output_unchanged=1 rebuild_ms=%.3f frame=%llu",
        (double)want,after[0],after[1],after[2],after[3],after[4],after[5],(double)(now_ns()-start)/1e6,(unsigned long long)dm_nvn_frame_count());
}
static void android_scale3d_tick(void *return_address) {
    if(android_s3_rebuilding || !dm_nvn_frame_count || dm_nvn_frame_count()<30)return;
    pthread_once(&android_s3_once,android_s3_configure);
    module_t *main=NULL;for(int i=0;i<g_nmod;i++)if(!strcmp(g_mod[i].name,"main")){main=&g_mod[i];break;}
    if(android_s3_identity_91738(main))android_scale3d_tick_91738(main,return_address);
    else android_scale3d_tick_94012(return_address);
}
