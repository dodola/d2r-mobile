#include "../host/dm.h"
#include "services_platform.h"
#include "services_sdk.h"
#include "services_fs.h"
#include "romfs_image.h"
#include "input_backend.h"
#include "render_scale.h"
#include "touch_controls_android.h"
#include "d2r_host_modules.h"
#include "d2r_mod_loader.h"
#ifdef DM_ANDROID_D2R_UI
#include "d2r_map_reveal.h"
#include "d2r_ui_adapter.h"
#endif
#ifdef DM_ANDROID_DEBUG_CONSOLE
#include "d2r_debug_mode.h"
#endif
#include "d2r_custom_android.h"
#include <jni.h>
#include "../host/dm_i18n.h"

/* Android UI 只取队列和译文；原游戏控件留在游戏线程。
 * Android UI reads only queued requests and translations; game widgets stay on the game thread. */
JNIEXPORT jboolean JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeShowFps(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;int dm_custom_get(const char *);return dm_custom_get("show_fps")!=0;
}
JNIEXPORT jboolean JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeFrontendTouch(JNIEnv *env,jclass cls,jint phase,jfloat x,jfloat y) {
    (void)env;(void)cls;
#ifdef DM_ANDROID_D2R_UI
    return dm_android_d2r_frontend_touch(phase,x,y)!=0;
#else
    (void)phase;(void)x;(void)y;
    return JNI_FALSE;
#endif
}
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeUiText(JNIEnv *env,jclass cls,jstring key) {
    (void)cls;if(!key)return NULL;const char *text=(*env)->GetStringUTFChars(env,key,NULL);if(!text)return NULL;
    jstring result=(*env)->NewStringUTF(env,dm_ui(text));(*env)->ReleaseStringUTFChars(env,key,text);return result;
}
#include "lifecycle_android.h"
#include "scale3d_android.h"
#include "../gfx/nvn/nvn_vk_memory.h"
#include "../host/dm_hud.h"
#include "../ios/Sources/dm_touch_game_state.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>
#include <math.h>
#include <dlfcn.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
extern void guest_thread_init(void);
extern void crash_thread_init(void);
extern uint32_t svc_thread_main_init(void);
extern int dm_android_diagnostics_flush(void);
extern void dm_vk_set_shader_root(const char *);
extern void dm_vk_set_window(void *);
extern void dm_vk_set_display_hz(int);
extern int dm_vk_attachment_applied_flags(void);
extern void dm_gfx_suspend(int);
extern void dm_audio_suspend(int);
extern void dm_focus_set(int);
extern void dm_ui_lifecycle(int background);
extern void dm_custom_init(const char *dir);
/* Optional diagnostic: game_resource_probe.c is enabled only when the
 * externally generated romfs_samples.h is supplied to CMake. */
extern void dm_android_real_resource_probe(const char *) __attribute__((weak));
extern void dm_host_shutdown(const char *why);
extern int dm_touch_game_attach(void);
extern uint64_t dm_fs_commit_count(void);
extern void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *) __attribute__((weak));
extern int dm_android_touch_visual_state(int *,int *,uint64_t *) __attribute__((weak));
extern void dm_android_touch_get_game_state(dm_android_touch_game_state *) __attribute__((weak));
extern int dm_padmode_current(void) __attribute__((weak));
extern int dm_padmode_busy(void) __attribute__((weak));
extern int dm_touch_game_snapshot(dm_tg_snapshot *) __attribute__((weak));
extern void dm_touch_game_diagnostics(dm_tg_diagnostics *) __attribute__((weak));

/* The update transport lives in Java while the build-number/notice widgets
 * remain in the shared title host. Keep one Activity reference for callbacks
 * from the guest's UI thread and release it with the Activity. */
static JavaVM *update_java_vm;
static jobject update_activity;
static pthread_mutex_t update_activity_lock = PTHREAD_MUTEX_INITIALIZER;
extern void dm_update_init(int, void (*)(void), void (*)(void));
extern void dm_update_offer(int, const char *, int);
extern void dm_update_fail(int);
extern void dm_update_foreground(void);
extern void dm_update_safe_left(int);
extern void dm_update_site(const char *);
#if !defined(DM_ANDROID_D2R_UI) || !DM_ANDROID_D2R_UI
/* The base 91738 package has no title-private update widgets. Keep the JNI
 * contract stable while the verified 94012 UI bridge supplies the implementation. */
void dm_update_init(int build, void (*select)(void), void (*click)(void)) {(void)build;(void)select;(void)click;}
void dm_update_offer(int build, const char *note, int urgent) {(void)build;(void)note;(void)urgent;}
void dm_update_fail(int kind) {(void)kind;}
void dm_update_foreground(void) {}
void dm_update_safe_left(int units) {(void)units;}
void dm_update_site(const char *host) {(void)host;}
#endif

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    update_java_vm=vm;
    return JNI_VERSION_1_6;
}

static JNIEnv *update_java_env(int *attached) {
    if (!update_java_vm) return NULL;
    JNIEnv *env=NULL;
    jint status=(*update_java_vm)->GetEnv(update_java_vm,(void **)&env,JNI_VERSION_1_6);
    if (status==JNI_OK) { *attached=0; return env; }
    if (status!=JNI_EDETACHED) return NULL;
    if ((*update_java_vm)->AttachCurrentThread(update_java_vm,&env,NULL)!=JNI_OK) return NULL;
    *attached=1;
    return env;
}

static void update_activity_call(const char *method_name) {
    int attached=0;
    JNIEnv *env=update_java_env(&attached);
    if (!env) return;
    jobject activity=NULL;
    pthread_mutex_lock(&update_activity_lock);
    if (update_activity) activity=(*env)->NewLocalRef(env,update_activity);
    pthread_mutex_unlock(&update_activity_lock);
    if (activity) {
        jclass cls=(*env)->GetObjectClass(env,activity);
        jmethodID method=cls?(*env)->GetMethodID(env,cls,method_name,"()V"):NULL;
        if (method) (*env)->CallVoidMethod(env,activity,method);
        if ((*env)->ExceptionCheck(env)) {
            LOG("Android update callback %s failed",method_name);
            (*env)->ExceptionClear(env);
        }
        if (cls) (*env)->DeleteLocalRef(env,cls);
        (*env)->DeleteLocalRef(env,activity);
    }
    if (attached) (*update_java_vm)->DetachCurrentThread(update_java_vm);
}

static void update_on_select(void) { update_activity_call("requestUpdateCheck"); }
static void update_on_click(void) { update_activity_call("openUpdatePage"); }

static void update_host(const char *url, char *host, size_t capacity) {
    host[0]='\0';
    if (!url || strncmp(url,"https://",8) || capacity<2) return;
    const char *start=url+8,*end=start;
    while (*end && *end!='/' && *end!=':' && *end!='?' && *end!='#') end++;
    size_t length=(size_t)(end-start);
    if (length>=capacity) length=capacity-1;
    memcpy(host,start,length);host[length]='\0';
}

JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeUpdateInit(
        JNIEnv *env,jobject self,jint own_build,jstring update_url) {
    jobject replacement=(*env)->NewGlobalRef(env,self);
    pthread_mutex_lock(&update_activity_lock);
    jobject previous=update_activity;
    update_activity=replacement;
    pthread_mutex_unlock(&update_activity_lock);
    if (previous) (*env)->DeleteGlobalRef(env,previous);
    char host[128]={0};
    const char *url=update_url?(*env)->GetStringUTFChars(env,update_url,NULL):NULL;
    update_host(url,host,sizeof host);
    if (update_url&&!url) return;
    dm_update_site(host);
    dm_update_safe_left(0);
    dm_update_init((int)own_build,update_on_select,update_on_click);
    if (update_url) (*env)->ReleaseStringUTFChars(env,update_url,url);
    LOG("Android update UI: build=%d configured=%d",(int)own_build,host[0]!=0);
}

JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeUpdateOffer(
        JNIEnv *env,jclass cls,jint build,jstring note,jboolean urgent) {
    (void)cls;
    const char *text=note?(*env)->GetStringUTFChars(env,note,NULL):NULL;
    if (note&&!text) return;
    dm_update_offer((int)build,text?text:"",urgent?1:0);
    if (note) (*env)->ReleaseStringUTFChars(env,note,text);
}

JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeUpdateFail(
        JNIEnv *env,jclass cls,jint kind) {
    (void)env;(void)cls;dm_update_fail((int)kind);
}

JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeUpdateForeground(
        JNIEnv *env,jclass cls) {
    (void)env;(void)cls;dm_update_foreground();
}

JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeUpdateShutdown(
        JNIEnv *env,jclass cls) {
    (void)cls;
    jobject previous=NULL;
    pthread_mutex_lock(&update_activity_lock);
    previous=update_activity;update_activity=NULL;
    pthread_mutex_unlock(&update_activity_lock);
    if (previous) (*env)->DeleteGlobalRef(env,previous);
}

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
/* Focus callbacks stay short. Save waits, filesystem work and shutdown
 * run on a dedicated worker; SurfaceView already owns a separate GPU worker. */
static pthread_mutex_t focus_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t lifecycle_cv = PTHREAD_COND_INITIALIZER;
static pthread_once_t lifecycle_once = PTHREAD_ONCE_INIT;
static int lifecycle_worker_error, activity_focus=1, shutdown_requested;
static uint64_t shutdown_save_epoch;
static uint64_t lifecycle_epoch=1, background_pending_epoch, background_pending_save_epoch;
static uint64_t background_wait_epoch, background_done_epoch;
static int foreground_pending;
static ANativeWindow *native_window;
static char app_files[PATH_MAX], port_root[PATH_MAX], locale_tag[32];
/* The verified image size and installed add-on indices are bound by
 * nativeContentProfile before guest startup; there is no default image. */
static uint64_t content_romfs_bytes;
static int content_indices[64];
static size_t content_index_count;
static char phase[96] = "等待游戏文件";
static _Atomic int configured, started, sdk_ready, main_entered, returned;
static _Atomic int input_route_ready, input_bound;
static _Atomic int lifecycle_focus = 1;
static _Atomic int audio_background, audio_interrupted;
static _Atomic uint64_t last_resign_save_ns;
static _Atomic uint64_t presented_frames;
static dm_android_window_state window_state = {.focus=3, .thermal_status=-1};
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static char report_file[PATH_MAX], report_temp[PATH_MAX];
static void path(char *out, size_t cap, const char *base, const char *leaf) {
    if (snprintf(out, cap, "%s/%s", base, leaf) >= (int)cap) die("Android private path too long");
}
/* Shared with the quality-mode menu. An absent file enables automatic
 * dynamic scaling; reject truncated values, NaN/Inf, and extra tokens. */
/* 把 diagnostics 下标记文件的内容（去掉首尾空白，至多 15 字节）放进环境变量；文件为空时用 fallback（NULL = 不设置）。
 * Put the content of a diagnostics marker file (whitespace trimmed, at most 15 bytes) into an environment variable; an empty
 * file uses fallback (NULL = leave unset). */
static void marker_to_env(const char *leaf, const char *name, const char *fallback) {
    char file[PATH_MAX], v[16] = {0};
    path(file, sizeof file, port_root, leaf);
    FILE *f = fopen(file, "r");
    if (!f) return;
    size_t n = fread(v, 1, 15, f);
    fclose(f);
    while (n && (v[n - 1] == 10 || v[n - 1] == 13 || v[n - 1] == 32)) v[--n] = 0;
    if (n) setenv(name, v, 1);
    else if (fallback) setenv(name, fallback, 1);
}
static int configure_render_scale(void) {
    char marker[PATH_MAX], text[64], value[32];
    path(marker,sizeof marker,port_root,"diagnostics/render-scale.txt");
    unsetenv("DM_3D_SCALE");
    FILE *f=fopen(marker,"r");
    if (!f) return errno==ENOENT?dm_android_render_scale_auto():errno;
    size_t n=fread(text,1,sizeof text-1,f);
    int error=ferror(f)?EIO:0;
    int trailing=fgetc(f);
    if (ferror(f)) error=EIO;
    if (fclose(f) && !error) error=errno?errno:EIO;
    if (error) return error;
    if (trailing!=EOF || memchr(text,'\0',n)) return EINVAL;
    text[n]='\0';
    char *start=text, *end;
    while (isspace((unsigned char)*start)) start++;
    if (!strncmp(start,"auto",4)) {
        end=start+4;
        while (isspace((unsigned char)*end)) end++;
        return *end?EINVAL:dm_android_render_scale_auto();
    }
    /* Match the menu's decimal format; do not accept C hex floats or suffixes. */
    for (size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)text[i];
        if (!isspace(c) && !isdigit(c) && c!='.' && c!='+' && c!='-' && c!='e' && c!='E') return EINVAL;
    }
    errno=0; float scale=strtof(start,&end);
    if (end==start || errno==ERANGE || !isfinite(scale) || scale<0.50f || scale>1.0f) return EINVAL;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return EINVAL;
    snprintf(value,sizeof value,"%.9g",(double)scale);
    if (setenv("DM_3D_SCALE",value,1)) return errno;
    return dm_android_render_scale_request(scale);
}
/* API 30+ native thermal service. Resolve dynamically so the API 29 shell
 * remains loadable; the process holds one manager for its lifetime. */
static pthread_once_t thermal_once=PTHREAD_ONCE_INIT;
static void *thermal_manager;
static int (*thermal_status)(void *);
static void thermal_start(void) {
    void *(*acquire)(void)=dlsym(RTLD_DEFAULT,"AThermal_acquireManager");
    thermal_status=dlsym(RTLD_DEFAULT,"AThermal_getCurrentThermalStatus");
    if(acquire && thermal_status)thermal_manager=acquire();
}
int dm_android_thermal_status_now(void) {
    pthread_once(&thermal_once,thermal_start);
    return thermal_manager && thermal_status ? thermal_status(thermal_manager) : -1;
}
static void status_text(char *text,size_t capacity) {
    char current_phase[sizeof phase];uint64_t epoch,done_epoch;
    pthread_mutex_lock(&state_lock);
    memcpy(current_phase,phase,sizeof current_phase);epoch=lifecycle_epoch;done_epoch=background_done_epoch;
    pthread_mutex_unlock(&state_lock);
    /* Keep all cross-subsystem reads outside state_lock. These APIs only
     * copy already-published state; status polling never calls a guest getter. */
    dm_hud_info hud;dm_hud_get(&hud);
    dm_android_input_status input;dm_android_input_status_get(&input);
    dm_android_touch_game_state touch={0};touch.pointer_item=-1;
    dm_android_d2r_scene_status scene={0};
    uint64_t touch_buttons=0;int skill_set=0,diff_open=0,touch_enabled=0;
    if(dm_android_touch_get_game_state)dm_android_touch_get_game_state(&touch);
    if(dm_android_touch_visual_state)touch_enabled=dm_android_touch_visual_state(&skill_set,&diff_open,&touch_buttons);
    if(dm_android_d2r_scene_status_get)dm_android_d2r_scene_status_get(&scene);
    dm_android_scale3d_status scale;dm_android_scale3d_status_get(&scale);
    DmVkMemoryStats memory={0};int memory_error=dm_vk_memory_get_stats(&memory);
    dm_tg_snapshot binding={0};dm_tg_diagnostics binding_diag={0};
    if(dm_touch_game_snapshot)dm_touch_game_snapshot(&binding);
    if(dm_touch_game_diagnostics)dm_touch_game_diagnostics(&binding_diag);
    uint64_t now=now_ns();int64_t binding_age=binding.captured_ns&&now>=binding.captured_ns?(int64_t)((now-binding.captured_ns)/1000000ull):-1;
    snprintf(text,capacity,"{\"phase\":\"%s\",\"sdk_ready\":%s,\"nnMain_entered\":%s,\"presented_frames\":%llu,\"returned\":%s,\"renderer_flags\":%d,\"render_scale\":%.4f,\"requested_render_scale\":%.4f,\"save_commits\":%llu,\"lifecycle_focus\":%d,\"lifecycle_epoch\":%llu,\"background_save_done_epoch\":%llu,"
        "\"hud\":{\"valid\":%d,\"area\":%d,\"level\":%d,\"hp\":%d,\"hp_max\":%d,\"mp\":%d,\"mp_max\":%d,\"x\":%d,\"y\":%d,\"dead\":%d},"
        "\"touch\":{\"enabled\":%d,\"navigation\":%d,\"panel_bits\":%d,\"buttons\":%llu,\"skill_set\":%d,\"diff_open\":%d},"
        "\"scene\":{\"attached\":%d,\"game\":%d,\"navigation\":%d,\"panels\":%llu,\"epoch\":%llu,\"world_epoch\":%llu,\"messages\":%llu},"
        "\"padmode\":{\"controller\":%d,\"layout\":%d,\"busy\":%d},"
        "\"binding\":{\"supported\":%d,\"valid\":%d,\"ready\":%d,\"source\":%d,\"scope\":%d,\"seen\":%u,\"captured_ns\":%llu,\"age_ms\":%lld,\"scene_epoch\":%llu,\"world_epoch\":%llu,\"identity\":%llu,\"caller\":%llu,\"hud\":%llu,\"bank\":%d,\"slot\":%d,\"observed\":%d,\"raw\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]},"
        "\"binding_diag\":{\"scene_game\":%d,\"cohort_confirmed\":%d,\"cohort_seen\":%u,\"pending_seen\":%u,\"scene_epoch\":%llu,\"world_epoch\":%llu,\"cohort_world\":%llu,\"cohort_hud\":%llu,\"last_reason\":%d,\"lr\":%llu,\"slot\":%u,\"stage\":[%u,%u,%u,%u,%u,%u,%u,%u],\"reason\":[%u,%u,%u,%u,%u,%u,%u,%u]},"
        "\"scale3d\":{\"supported\":%d,\"quality\":%d,\"level\":%d,\"configured\":%.3f,\"current\":%.3f,\"floor\":%.3f,\"internal\":[%u,%u],\"output\":[%u,%u],\"target_ms\":%.3f,\"interval_ms\":%.3f,\"thermal\":%d,\"changes\":%llu,\"hold_until\":%llu,\"source\":\"queue-present-return\",\"scanout_completion\":false},"
        "\"gpu_memory\":{\"available\":%s,\"error\":%d,\"allocation_bytes\":%llu,\"peak_allocation_bytes\":%llu,\"texture\":%llu,\"sample_copy\":%llu,\"shadow\":%llu,\"pool\":%llu,\"support\":%llu,\"temp\":%llu,\"writeback\":%llu,\"readback\":%llu,\"present\":%llu,\"cpu_baseline\":%llu,\"temp_recording\":%llu,\"temp_inflight\":%llu,\"temp_retired\":%llu,\"trims\":%llu,\"last_trim_freed\":%llu},"
        "\"input\":{\"ready\":%d,\"initialized\":%d,\"publications\":%llu,\"samples\":%llu,\"action_buttons\":%llu,\"controller_buttons\":%llu,\"raw_buttons\":%llu,\"buttons\":%llu,\"buttons_seen\":%llu,\"nonzero_samples\":%llu,\"attributes\":%u,\"id\":%u,\"style\":%u,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d}}",current_phase,
        atomic_load(&sdk_ready)?"true":"false",atomic_load(&main_entered)?"true":"false",
        (unsigned long long)atomic_load(&presented_frames),atomic_load(&returned)?"true":"false",
        dm_vk_attachment_applied_flags(),(double)dm_3d_scale_cur(),(double)dm_android_render_scale_requested(),
        (unsigned long long)dm_fs_commit_count(),atomic_load(&lifecycle_focus),
        (unsigned long long)epoch,(unsigned long long)done_epoch,
        hud.valid,hud.area,hud.level,hud.hp,hud.hp_max,hud.mp,hud.mp_max,hud.x,hud.y,hud.dead,
        touch_enabled,touch.navigation,touch.panel_bits,(unsigned long long)touch_buttons,skill_set,diff_open,
        scene.attached,scene.game,scene.navigation,(unsigned long long)scene.panels,
        (unsigned long long)scene.scene_epoch,(unsigned long long)scene.world_epoch,(unsigned long long)scene.uimsg_count,
        input.controller_mode,dm_padmode_current?dm_padmode_current():-1,dm_padmode_busy?dm_padmode_busy():0,
        binding.supported,binding.valid,binding.ready,binding.source,binding.identity_scope,binding.group_seen,
        (unsigned long long)binding.captured_ns,(long long)binding_age,
        (unsigned long long)binding.scene_epoch,(unsigned long long)binding.world_epoch,
        (unsigned long long)binding.binding_identity,(unsigned long long)binding.caller_identity,(unsigned long long)binding.hud_identity,
        binding.selected_bank,binding.getter_input_slot,binding.observed_index,
        binding.raw_skill_id[0],binding.raw_skill_id[1],binding.raw_skill_id[2],binding.raw_skill_id[3],binding.raw_skill_id[4],binding.raw_skill_id[5],
        binding.raw_skill_id[6],binding.raw_skill_id[7],binding.raw_skill_id[8],binding.raw_skill_id[9],binding.raw_skill_id[10],binding.raw_skill_id[11],
        binding_diag.scene_game,binding_diag.cohort_confirmed,binding_diag.cohort_seen,binding_diag.pending_seen,
        (unsigned long long)binding_diag.scene_epoch,(unsigned long long)binding_diag.world_epoch,
        (unsigned long long)binding_diag.cohort_world_epoch,(unsigned long long)binding_diag.cohort_hud_identity,
        binding_diag.last_identity_reason,(unsigned long long)binding_diag.last_identity_lr,binding_diag.last_identity_slot,
        binding_diag.stage[0],binding_diag.stage[1],binding_diag.stage[2],binding_diag.stage[3],binding_diag.stage[4],binding_diag.stage[5],binding_diag.stage[6],binding_diag.stage[7],
        binding_diag.reason[0],binding_diag.reason[1],binding_diag.reason[2],binding_diag.reason[3],binding_diag.reason[4],binding_diag.reason[5],binding_diag.reason[6],binding_diag.reason[7],
        scale.supported,scale.quality,scale.level,(double)scale.configured,(double)scale.current,(double)scale.floor,
        scale.internal_width,scale.internal_height,scale.output_width,scale.output_height,(double)scale.target_period_ns/1e6,scale.interval_ms,
        scale.thermal_status,(unsigned long long)scale.changes,(unsigned long long)scale.hold_until,
        memory_error?"false":"true",memory_error,
        (unsigned long long)memory.total_bytes,(unsigned long long)memory.peak_total_bytes,
        (unsigned long long)memory.bytes[DM_VK_MEMORY_TEXTURE],(unsigned long long)memory.bytes[DM_VK_MEMORY_SAMPLE_COPY],
        (unsigned long long)memory.bytes[DM_VK_MEMORY_SHADOW],(unsigned long long)memory.bytes[DM_VK_MEMORY_POOL],
        (unsigned long long)memory.bytes[DM_VK_MEMORY_SUPPORT],(unsigned long long)memory.bytes[DM_VK_MEMORY_TEMP],
        (unsigned long long)memory.bytes[DM_VK_MEMORY_WRITEBACK],(unsigned long long)memory.bytes[DM_VK_MEMORY_READBACK],
        (unsigned long long)memory.bytes[DM_VK_MEMORY_PRESENT],(unsigned long long)memory.cpu_baseline_bytes,
        (unsigned long long)memory.temp_recording_bytes,(unsigned long long)memory.temp_inflight_bytes,
        (unsigned long long)memory.temp_retired_bytes,(unsigned long long)memory.trim_count,(unsigned long long)memory.last_trim_freed_bytes,
        input.source_ready,input.npad_initialized,(unsigned long long)input.action_publications,
        (unsigned long long)input.npad_samples,(unsigned long long)input.action_buttons,
        (unsigned long long)input.controller_buttons,(unsigned long long)input.raw_buttons,
        (unsigned long long)input.returned_state.buttons,(unsigned long long)input.returned_buttons_seen,
        (unsigned long long)input.returned_nonzero_samples,input.returned_state.attributes,input.returned_id,input.returned_style,
        input.returned_state.lx,input.returned_state.ly,input.returned_state.rx,input.returned_state.ry);
}
static void persist_status(void) {
    if (!report_file[0]) return;
    pthread_mutex_lock(&report_lock);
    char text[8192];status_text(text,sizeof text);
    int fd=open(report_temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600),error=0;
    if(fd<0)error=errno;
    else {size_t n=strlen(text),sent=0;while(sent<n){ssize_t k=write(fd,text+sent,n-sent);if(k<0&&errno==EINTR)continue;if(k<=0){error=errno?errno:EIO;break;}sent+=(size_t)k;}
        if(!error&&fsync(fd))error=errno;if(close(fd)&&!error)error=errno;
        if(!error&&rename(report_temp,report_file))error=errno;}
    if(error)LOG("ANDROID_STATUS_WRITE_FAILED errno=%d",error);
    pthread_mutex_unlock(&report_lock);
}
static void phase_set(const char *text) {
    pthread_mutex_lock(&state_lock); snprintf(phase, sizeof phase, "%s", text); pthread_mutex_unlock(&state_lock);
    LOG("ANDROID_GAME_STAGE %s", text);
    persist_status();
}
void *dm_android_native_window(void) {
    pthread_mutex_lock(&state_lock); ANativeWindow *w = native_window; pthread_mutex_unlock(&state_lock);
    return w; /* Root owns an acquired reference until graphics are suspended/drained. */
}
void dm_android_front_presented(uint64_t frame) {
    atomic_store(&presented_frames, frame);
    if (frame == 1) phase_set("游戏画面已呈现");
}
static int suspend_graphics(int on) { dm_gfx_suspend(on); return 0; }
static void android_audio_apply(void) {
    dm_audio_suspend(atomic_load(&audio_background) || atomic_load(&audio_interrupted));
}
static int pause_audio(int on) {
    atomic_store(&audio_background, !!on);
    android_audio_apply();
    return 0;
}
static int background_current(uint64_t epoch) {
    pthread_mutex_lock(&state_lock);
    int current=!shutdown_requested && lifecycle_epoch==epoch && atomic_load(&lifecycle_focus)==3;
    pthread_mutex_unlock(&state_lock); return current;
}
static void *lifecycle_worker(void *unused) {
    (void)unused; crash_thread_init();
    for (;;) {
        pthread_mutex_lock(&state_lock);
        while (shutdown_requested==1 || (!shutdown_requested && !background_pending_epoch && !foreground_pending))
            pthread_cond_wait(&lifecycle_cv,&state_lock);
        int shutdown=shutdown_requested;
        foreground_pending=0;
        uint64_t epoch=background_pending_epoch, save_epoch=background_pending_save_epoch;
        uint64_t final_save_epoch=shutdown_save_epoch;
        background_pending_epoch=0;
        pthread_mutex_unlock(&state_lock);
        if (shutdown) {
            /* onDestroy may follow onPause before its asynchronous save ran.
             * Rollback/cancel completes first, then make one final bounded
             * request on this worker before backend teardown exits. */
            if (atomic_load(&started) && dm_ingame_now()) {
                uint64_t commits=0; int froze=0; double elapsed=0;
                dm_android_save_wait_current(final_save_epoch,2.5,&commits,&froze,&elapsed);
                LOG("Android lifecycle: destroy save wait %.0f ms commits=%llu froze=%d",elapsed,
                    (unsigned long long)commits,froze);
            }
            /* Shutdown's backend watchdog supplies the hard exit deadline;
             * never join a potentially blocked provider on the UI thread. */
            if (atomic_load(&started)) dm_host_shutdown("Android Activity destroyed");
            return NULL;
        }
        if (!epoch) continue;
        if (background_current(epoch)) {
            uint64_t commits=0; int froze=0; double elapsed=0;
            if (dm_ingame_now()) {
                dm_android_save_wait_current(save_epoch,2.5,&commits,&froze,&elapsed);
                LOG("Android lifecycle: background save wait %.0f ms commits=%llu froze=%d epoch=%llu",elapsed,
                    (unsigned long long)commits,froze,(unsigned long long)epoch);
            } else LOG("Android lifecycle: background outside run, save wait skipped");
            if (background_current(epoch)) {
                const char *save=getenv("DM_SAVE");
                if (save && dm_android_fs_sync_tree(save)) LOG("Android lifecycle: save tree sync failed errno=%d",errno);
            }
        }
        pthread_mutex_lock(&state_lock);
        /* The focus write and this check/apply share the same mutex. A stale
         * background job cannot freeze a resumed app, even after another
         * background/foreground round trip reused focus=3. */
        if (!shutdown_requested && lifecycle_epoch==epoch && atomic_load(&lifecycle_focus)==3)
            guest_clock_freeze_for(1,1);
        if (background_done_epoch<epoch) background_done_epoch=epoch;
        pthread_cond_broadcast(&lifecycle_cv);
        pthread_mutex_unlock(&state_lock);
        /* Publish save completion from its worker as well, independent of
         * Java's polling cadence or Activity lifecycle. */
        persist_status();
    }
}
static void start_lifecycle_worker(void) {
    pthread_t worker;
    lifecycle_worker_error=pthread_create(&worker,NULL,lifecycle_worker,NULL);
    if (!lifecycle_worker_error) pthread_detach(worker);
}
/* Caller owns focus_lock. No GPU drain, file sync, provider lock or commit
 * wait is performed here. activity_intent=-1 preserves the Activity's intent
 * when a delayed Surface callback publishes its actual window. */
static void apply_focus_locked(int focus,int activity_intent) {
    pthread_mutex_lock(&state_lock);
    if (shutdown_requested) { pthread_mutex_unlock(&state_lock); return; }
    int changed_intent=activity_intent>=0 && activity_focus!=activity_intent;
    if (activity_intent>=0) activity_focus=activity_intent;
    int previous=atomic_exchange(&lifecycle_focus,focus);
    if (previous!=focus || changed_intent) ++lifecycle_epoch;
    uint64_t epoch=lifecycle_epoch;
    if (focus!=3) { background_pending_epoch=0; foreground_pending=1; }
    pthread_cond_broadcast(&lifecycle_cv);
    pthread_mutex_unlock(&state_lock);
    if (focus!=3) {
        /* Invalidate queued work and restore pause before releasing clock. */
        dm_android_save_cancel();
        pthread_mutex_lock(&state_lock);
        if (!shutdown_requested && lifecycle_epoch==epoch && atomic_load(&lifecycle_focus)!=3)
            guest_clock_freeze_for(1,0);
        pthread_mutex_unlock(&state_lock);
    }
    if ((previous==focus && !changed_intent) || !atomic_load(&started)) return;
    if (focus!=1) {
        dm_android_input_cancel(); dm_focus_set(focus);
        if (focus==3) dm_ui_lifecycle(1);
        uint64_t now=now_ns(), last=atomic_load(&last_resign_save_ns);
        if ((!last || now-last>=30000000000ull) && dm_request_save())
            atomic_store(&last_resign_save_ns,now);
    } else { dm_ui_lifecycle(0); dm_focus_set(1); }
    atomic_store(&audio_background,focus!=1); android_audio_apply();
    pthread_once(&lifecycle_once,start_lifecycle_worker);
    if (lifecycle_worker_error) { LOG("Android lifecycle: worker creation failed errno=%d",lifecycle_worker_error); return; }
    pthread_mutex_lock(&state_lock);
    if (focus==3 && lifecycle_epoch==epoch && !shutdown_requested) {
        background_wait_epoch=background_pending_epoch=epoch;
        background_pending_save_epoch=dm_android_save_epoch();
    }
    pthread_cond_broadcast(&lifecycle_cv);
    pthread_mutex_unlock(&state_lock);
}
static void android_focus_transition(int focus,int activity_intent) {
    pthread_mutex_lock(&focus_lock); apply_focus_locked(focus,activity_intent); pthread_mutex_unlock(&focus_lock);
}
static int surface_focus_locked(int has_window) {
    pthread_mutex_lock(&state_lock);
    int focus=activity_focus==1?(has_window?1:2):activity_focus;
    pthread_mutex_unlock(&state_lock);
    apply_focus_locked(focus,-1); return focus;
}
/* Explicit lifecycle entry used by the Java Activity. Surface callbacks are
 * still authoritative for the actual window, but Android may pause an
 * Activity before SurfaceView reports destruction; keeping this hook mirrors
 * the iOS scene events and guarantees the save request is issued first. */
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeLifecycle(JNIEnv *env,jclass cls,jint event) {
    (void)env; (void)cls;
    if (event < 0 || event > 5) return EINVAL;
    switch (event) {
        case 0: android_focus_transition(2,2); break; /* resign active */
        case 1: android_focus_transition(3,3); break; /* queue background work */
        case 2: android_focus_transition(2,1); break; /* foreground; Surface activates */
        case 3: android_focus_transition(1,1); break;
        case 4: atomic_store(&audio_interrupted,1); android_audio_apply(); LOG("Android lifecycle: audio focus interrupted"); break;
        case 5: atomic_store(&audio_interrupted,0); android_audio_apply(); LOG("Android lifecycle: audio focus resumed"); break;
    }
    return 0;
}
static int input_source_ready(void *opaque) {(void)opaque;return atomic_load(&input_route_ready)&&dm_android_native_window()!=NULL;}
static void *required(const char *name) {
    void *fn = guest_sym(name); if (!fn) die("Missing original startup export %s", name); return fn;
}
static void call_original(const char *name) {
    LOG("ANDROID_ORIGINAL_CALL %s", name); ((void (*)(void))required(name))();
    LOG("ANDROID_ORIGINAL_RETURN %s", name);
}
static void *game_thread(void *unused) {
    (void)unused;
    crash_thread_init();
    char debug_hold[PATH_MAX];path(debug_hold,sizeof debug_hold,port_root,"diagnostics/debug-hold");
    if(!access(debug_hold,F_OK)) { LOG("ANDROID_DEBUG_HOLD before original startup");raise(SIGSTOP); }
    char modules[PATH_MAX], rom[PATH_MAX], image[PATH_MAX], save[PATH_MAX], temp[PATH_MAX], profile[PATH_MAX];
    path(modules,sizeof modules,port_root,"modules"); path(rom,sizeof rom,port_root,"romfs-view");
    path(image,sizeof image,app_files,"game.romfs"); path(save,sizeof save,port_root,"save");
    path(temp,sizeof temp,port_root,"temp"); path(profile,sizeof profile,port_root,"profile");
    pthread_mutex_lock(&state_lock);
    uint64_t expected_image_bytes=content_romfs_bytes;
    pthread_mutex_unlock(&state_lock);
    struct stat st;
    if (stat(image,&st) || st.st_size < 0 || (uint64_t)st.st_size != expected_image_bytes)
        die("Original RomFS image absent or differs from the verified content manifest");
    phase_set("正在读取游戏资源");
    int error = romfs_image_setup_checked(image,rom,expected_image_bytes);
    if (error) die("RomFS image setup errno=%d",errno);
#ifdef DM_ANDROID_DEBUG_CONSOLE
    if (dm_android_d2r_debug_configure(port_root,save,sizeof save)) die("Android debug configuration failed");
#endif
    setenv("DM_ROMFS",rom,1); setenv("DM_TEMP",temp,1);
    if (locale_tag[0]) setenv("DM_LANG", locale_tag, 1);
    /* Keep the portable Custom settings store beside the app's save tree,
     * matching iOS's Documents-level placement and making it available
     * before the guest mounts its save. */
    dm_custom_init(port_root);
    const char *custom_locale=NULL;
    char overlay[4096]="";
#ifdef DM_ANDROID_D2R_UI
    dm_android_custom_overlay(port_root,overlay,sizeof overlay);
    custom_locale=dm_android_custom_startup_locale();
#endif
    error=dm_android_mod_configure(port_root,overlay,sizeof overlay,save,sizeof save);
    if(error)die("Android MOD package invalid or base overlay differs, error=%d; clear mods/active.txt to recover",error);
    if(overlay[0])setenv("DM_ROMFS_OVERLAY",overlay,1);else unsetenv("DM_ROMFS_OVERLAY");
    if (dm_android_fs_ensure_directory(save) || dm_android_fs_ensure_directory(temp) || dm_android_fs_ensure_directory(profile)) die("Android private directories");
    setenv("DM_SAVE",save,1);
    const char *startup_locale=custom_locale?custom_locale:locale_tag;
    if(custom_locale) {setenv("DM_LANG",custom_locale,1);LOG("ANDROID_CUSTOM_LANGUAGE startup_locale=%s source=sw_custom.txt",custom_locale);}
    char private_vulkan[PATH_MAX];path(private_vulkan,sizeof private_vulkan,port_root,"diagnostics/private-vulkan");
    if(access(private_vulkan,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    dm_android_platform_config config = {.mode=DM_ANDROID_HOST_ACTIVITY,
        .profile_directory=profile, .locale=startup_locale, .locale_source=custom_locale?"Custom.lang_en":"Activity.Configuration",
        .add_on_content_indices=content_indices, .add_on_content_count=content_index_count,
        .content_manifest_verified=1, .graphics_present=1, .audio_present=1,
        .suspend_and_drain_graphics=suspend_graphics, .pause_audio=pause_audio, .flush_diagnostics=dm_android_diagnostics_flush};
    if ((error=dm_android_platform_bind(&config)) || (error=dm_android_platform_register_services())) die("Android platform bind/register error=%d",error);
    pthread_mutex_lock(&state_lock); dm_android_window_state w=window_state; pthread_mutex_unlock(&state_lock);
    if ((error=dm_android_platform_update_window(&w))) die("Android initial window state error=%d",error);
    dm_android_service_register(DM_ANDROID_SERVICE_FS,dm_android_fs_initialize,NULL);
    phase_set("正在装载游戏");
    const char *names[]={"main","sdk","subsdk0"}; load_modules(modules,names,3);
    /* D2R's binding observer is an optional, build-gated title adapter.  It
     * only installs when the loaded artifact advertises OBS_BINDING_GETTER
     * and the exact module size/identity checks pass; unreviewed titles do not
     * link this adapter and never reach this path. */
#ifdef DM_ANDROID_D2R_UI
    { int binding_attached=dm_touch_game_attach();
      int dm_android_d2r_host_attach(void); int host_attached=dm_android_d2r_host_attach();
      void dm_touch_game_live_check(int); dm_touch_game_live_check(binding_attached && host_attached);
      LOG("ANDROID_TOUCH_GAME_ATTACH result=%d host=%d live_check=%d",binding_attached,host_attached,binding_attached && host_attached); }
#endif
    guest_thread_init();
    uint32_t handle=svc_thread_main_init();
    phase_set("正在初始化游戏"); call_original("_init_libc0");
    LOG("ANDROID_ORIGINAL_CALL nnosInitialize handle=%u",handle);
    ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(handle,0);
    LOG("ANDROID_ORIGINAL_RETURN nnosInitialize");
    call_original("_init_libc1"); call_original("nninitInitializeSdkModule");
    call_original("nninitStartup"); call_original("_init_libc2"); atomic_store(&sdk_ready,1);
    if (dm_android_real_resource_probe) dm_android_real_resource_probe(port_root);
    else LOG("ANDROID_REAL_RESOURCE_READ skipped (DM_ANDROID_ROMFS_SAMPLES_HEADER not supplied)");
    phase_set("正在启动游戏"); run_module_inits();
    char *args[]={"D2RMobile",NULL};
    ((void (*)(int))required("_ZN2nn2os11SetHostArgcEi"))(1);
    ((void (*)(char **))required("_ZN2nn2os11SetHostArgvEPPc"))(args);
    atomic_store(&main_entered,1);persist_status();call_original("nnMain");
    atomic_store(&returned,1); phase_set("游戏已退出"); return NULL;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeContentProfile(JNIEnv *env,jclass cls,jlong bytes,jintArray indices) {
    (void)cls;
    if (bytes < 0x50 || !indices) return EINVAL;
    jsize count=(*env)->GetArrayLength(env,indices);
    if (count < 0 || count > 64) return EINVAL;
    jint values[64];
    if (count) (*env)->GetIntArrayRegion(env,indices,0,count,values);
    if ((*env)->ExceptionCheck(env)) return EINVAL;
    for (jsize i=0;i<count;++i) {
        if (values[i]<0) return EINVAL;
        for (jsize j=0;j<i;++j) if (values[j]==values[i]) return EINVAL;
    }
    pthread_mutex_lock(&state_lock);
    if (atomic_load(&started)) { pthread_mutex_unlock(&state_lock); return EBUSY; }
    content_romfs_bytes=(uint64_t)bytes;
    content_index_count=(size_t)count;
    for (jsize i=0;i<count;++i) content_indices[i]=values[i];
    pthread_mutex_unlock(&state_lock);
    LOG("ANDROID_CONTENT_PROFILE romfs_bytes=%llu add_on_count=%d",(unsigned long long)bytes,(int)count);
    return 0;
}
static int copy_jstring(JNIEnv *env,jstring text,char *out,size_t cap) {
    if (!text) return EINVAL;
    const char *s=(*env)->GetStringUTFChars(env,text,NULL); if (!s) return ENOMEM;
    int rc=strlen(s)>=cap?ENAMETOOLONG:0; if (!rc) strcpy(out,s);
    (*env)->ReleaseStringUTFChars(env,text,s); return rc;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeConfigure(JNIEnv *env,jclass cls,jstring files,jstring locale,jboolean touch_route) {
    (void)cls;
    if (atomic_exchange(&configured,1)) return EALREADY;
    int rc=copy_jstring(env,files,app_files,sizeof app_files);
    if (!rc) rc=copy_jstring(env,locale,locale_tag,sizeof locale_tag);
    if (rc || app_files[0]!='/') return rc?rc:EINVAL;
    if (!touch_route) return ENODEV;
    atomic_store(&input_route_ready,1);
    path(port_root,sizeof port_root,app_files,"d2r-mobile");
    if ((rc=configure_render_scale())) { LOG("ANDROID_3D_SCALE_CONFIG_FAILED errno=%d expected finite 0.50..1.0",rc); return rc; }
    char driver_option[PATH_MAX];
    unsetenv("DM_ANDROID_PRESENT_60FPS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/present-60fps");
    if(access(driver_option,F_OK)==0)setenv("DM_ANDROID_PRESENT_60FPS","1",1);
    unsetenv("DM_DISABLE_PRESENT_60FPS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-present-60fps");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PRESENT_60FPS","1",1);
    unsetenv("DM_DISABLE_PRESENT_CADENCE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-present-cadence");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PRESENT_CADENCE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan");
    if(access(driver_option,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan-sysmem");
    if(access(driver_option,F_OK)==0){setenv("DM_PRIVATE_VULKAN","turnip",1);setenv("TU_DEBUG","sysmem",1);}
    path(driver_option,sizeof driver_option,port_root,"diagnostics/turnip-debug.txt");
    if(getenv("DM_PRIVATE_VULKAN")&&!strcmp(getenv("DM_PRIVATE_VULKAN"),"turnip")){
        FILE *tf=fopen(driver_option,"r");
        if(tf){
            char value[64];size_t n=fread(value,1,sizeof value-1,tf);int extra=fgetc(tf);int read_error=ferror(tf);fclose(tf);
            if(!read_error&&extra==EOF&&n<sizeof value){
                size_t begin=0,end=n;
                while(begin<end&&isspace((unsigned char)value[begin]))begin++;
                while(end>begin&&isspace((unsigned char)value[end-1]))end--;
                int valid=end>begin&&end-begin<sizeof value;
                for(size_t i=begin;i<end&&valid;i++){
                    unsigned char c=(unsigned char)value[i];
                    if(!(islower(c)||isdigit(c)||c=='_'||c==','))valid=0;
                }
                if(valid){value[end]=0;setenv("TU_DEBUG",value+begin,1);
                    LOG("ANDROID_TURNIP_DEBUG options=%s",value+begin);}
                else LOG("ANDROID_TURNIP_DEBUG invalid file ignored");
            }else LOG("ANDROID_TURNIP_DEBUG invalid file ignored");
        }
    }
    path(driver_option,sizeof driver_option,port_root,"diagnostics/vulkan-validation");
    if(access(driver_option,F_OK)==0)setenv("DM_VULKAN_VALIDATION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pass-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PASS_REUSE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-cpu-uniform-snapshot");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_CPU_UNIFORM_SNAPSHOT","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pso-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PSO_REUSE","1",1);
    unsetenv("DM_PSO_LAST_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-pso-last-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_PSO_LAST_CACHE","1",1);
    setenv("DM_WRITEBACK_PREPARE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-writeback-prepare");
    if(access(driver_option,F_OK)==0)setenv("DM_WRITEBACK_PREPARE","0",1);
    unsetenv("DM_WRITEBACK_COALESCE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/writeback-coalesce");
    if(access(driver_option,F_OK)==0)setenv("DM_WRITEBACK_COALESCE","1",1);
    /* Exact state reuse is safe within a command buffer and materially cuts
     * replay work on the 1280x720 model-heavy scene. Keep the diagnostic
     * marker for a conservative A/B fallback. */
    setenv("DM_COMMAND_STATE_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-command-state-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_COMMAND_STATE_CACHE","0",1);
    setenv("DM_DIRTY_SUMMARY","1",1);unsetenv("DM_DIRTY_QUERY_STATS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dirty-summary");
    if(access(driver_option,F_OK)==0)setenv("DM_DIRTY_SUMMARY","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/dirty-query-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_DIRTY_QUERY_STATS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-driver-table-reuse");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DRIVER_TABLE_REUSE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-temp-pool");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_TEMP_POOL","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-storage-bias");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_STORAGE_BIAS","1",1);
    unsetenv("DM_DIAG_IMG_USAGE");
    unsetenv("DM_POOL_RANGE_STATS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pool-range-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_POOL_RANGE_STATS","1",1);
    unsetenv("DM_POOL_OWNED_READ_BYPASS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pool-owned-read-bypass");
    if(access(driver_option,F_OK)==0)setenv("DM_POOL_OWNED_READ_BYPASS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-pool-owned-read-bypass");
    if(access(driver_option,F_OK)==0)setenv("DM_POOL_OWNED_READ_BYPASS","0",1);
    unsetenv("DM_VK_IMAGE_USAGE_FLAGS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/declared-image-usage");
    if(access(driver_option,F_OK)==0)setenv("DM_VK_IMAGE_USAGE_FLAGS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/conservative-image-usage");
    if(access(driver_option,F_OK)==0)setenv("DM_VK_IMAGE_USAGE_FLAGS","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-img-usage-1");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_IMG_USAGE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-img-usage-2");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_IMG_USAGE","2",1);
    unsetenv("DM_BARRIER_PASS_ELIDE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/barrier-pass-elide");
    if(access(driver_option,F_OK)==0)setenv("DM_BARRIER_PASS_ELIDE","1",1);
    unsetenv("DM_DIAG_NO_PASS_DEPS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-no-pass-deps");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_NO_PASS_DEPS","1",1);
    unsetenv("DM_DISABLE_PRECISE_SYNC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-precise-sync");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_PRECISE_SYNC","1",1);
    unsetenv("DM_HZ_DIAG");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/hz-diag.txt");
    {FILE *hf=fopen(driver_option,"r");if(hf){char hv[8]={0};size_t hn=fread(hv,1,7,hf);fclose(hf);if(hn)setenv("DM_HZ_DIAG",hv,1);}}
    unsetenv("DM_CHUNK_DRAWS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/chunk-draws.txt");
    {FILE *cf=fopen(driver_option,"r");if(cf){char cv[12]={0};size_t cn=fread(cv,1,11,cf);fclose(cf);if(cn)setenv("DM_CHUNK_DRAWS",cv,1);}}
    unsetenv("DM_VK_CHUNK_CONTROL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/chunk-control.txt");
    if(access(driver_option,F_OK)==0)setenv("DM_VK_CHUNK_CONTROL",driver_option,1);
    unsetenv("DM_PROFILE_DETAIL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/profile-detail");
    if(access(driver_option,F_OK)==0)setenv("DM_PROFILE_DETAIL","1",1);
    unsetenv("DM_DIAG_SKIP_DRAWS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-skip-draws");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_SKIP_DRAWS","1",1);
    unsetenv("DM_DIAG_SKIP_XFER");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-xfer.txt");
    {FILE *xf=fopen(driver_option,"r");if(xf){char xv[8]={0};size_t xn=fread(xv,1,7,xf);fclose(xf);if(xn)setenv("DM_DIAG_SKIP_XFER",xv,1);}}
    unsetenv("DM_DISABLE_SUBRES");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-subres");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SUBRES","1",1);
    unsetenv("DM_DISABLE_SETRT_SKIP");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-setrt-skip");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SETRT_SKIP","1",1);
    unsetenv("DM_DISABLE_WB_STAGE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-wb-stage");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_WB_STAGE","1",1);
    unsetenv("DM_DIAG_SKIP_CLASS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-class.txt");
    {FILE *kf=fopen(driver_option,"r");if(kf){char kv[8]={0};size_t kn=fread(kv,1,7,kf);fclose(kf);if(kn)setenv("DM_DIAG_SKIP_CLASS",kv,1);}}
    unsetenv("DM_DIAG_SHAPE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-shape.txt");
    {FILE *hf2=fopen(driver_option,"r");if(hf2){char hv2[8]={0};size_t hn2=fread(hv2,1,7,hf2);fclose(hf2);if(hn2)setenv("DM_DIAG_SHAPE",hv2,1);}}
    unsetenv("DM_NARROW_ACCESS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/narrow-access");
    if(access(driver_option,F_OK)==0)setenv("DM_NARROW_ACCESS","1",1);
    unsetenv("DM_DISABLE_WAIT_ELISION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-wait-elision");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_WAIT_ELISION","1",1);
    unsetenv("DM_DISABLE_ASYNC_COMMIT");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-async-commit");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_ASYNC_COMMIT","1",1);
    unsetenv("DM_BC_PASSTHROUGH");unsetenv("DM_YIELD_BACKOFF");unsetenv("DM_TEXTURE_UPLOAD_BATCH");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-bc-passthrough");
    if(access(driver_option,F_OK)==0)setenv("DM_BC_PASSTHROUGH","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-yield-backoff");
    if(access(driver_option,F_OK)==0)setenv("DM_YIELD_BACKOFF","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-texture-upload-batch");
    if(access(driver_option,F_OK)==0)setenv("DM_TEXTURE_UPLOAD_BATCH","0",1);
    unsetenv("DM_NVN_SYNC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/nvn-sync");
    if(access(driver_option,F_OK)==0)setenv("DM_NVN_SYNC","1",1);
    unsetenv("DM_CONFLICT_WAIT_ALL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/conflict-wait-all");
    if(access(driver_option,F_OK)==0)setenv("DM_CONFLICT_WAIT_ALL","1",1);
    unsetenv("DM_DIAG_FIXED_DESC");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-fixed-desc");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_FIXED_DESC","1",1);
    unsetenv("DM_DIAG_NO_OCCLUSION");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-no-occlusion");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_NO_OCCLUSION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-query-batch");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_QUERY_BATCH","1",1);
    setenv("DM_QUERY_RESET_RANGE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-query-reset-range");
    if(access(driver_option,F_OK)==0)setenv("DM_QUERY_RESET_RANGE","0",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-desc-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DESC_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-resource-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_RESOURCE_CACHE","1",1);
    unsetenv("DM_VK_RESOURCE_CONTROL");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/resource-control.txt");
    if(access(driver_option,F_OK)==0)setenv("DM_VK_RESOURCE_CONTROL",driver_option,1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dirty-uploads");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DIRTY_UPLOADS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-control-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_CONTROL_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-loadstore-store");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_LOADSTORE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-loadstore-both");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_LOADSTORE","2",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-barrier-elision");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_BARRIER_ELISION","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-sample-copy-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SAMPLE_COPY_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/skip-ps.txt");
    {FILE *sf=fopen(driver_option,"r");if(sf){char sk[17]={0};size_t n=fread(sk,1,16,sf);fclose(sf);if(n==16)setenv("DM_SKIP_PS",sk,1);}}
    unsetenv("DM_EVENT_TRACE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/event-trace");
    if(access(driver_option,F_OK)==0)setenv("DM_EVENT_TRACE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pass-timing");
    if(access(driver_option,F_OK)==0)setenv("DM_PASS_TIMING","1",1);
    unsetenv("DM_PROTECT_AROUND");
    marker_to_env("diagnostics/protect-around.txt","DM_PROTECT_AROUND",NULL);
    unsetenv("DM_FAULT_AROUND");
    marker_to_env("diagnostics/fault-around.txt","DM_FAULT_AROUND",NULL);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-fault-around");
    if(access(driver_option,F_OK)==0){setenv("DM_FAULT_AROUND","1",1);setenv("DM_PROTECT_AROUND","1",1);}
    unsetenv("DM_ENABLE_DYNAMIC_UBO");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-dynamic-ubo");
    if(access(driver_option,F_OK)==0)setenv("DM_ENABLE_DYNAMIC_UBO","1",1);
    unsetenv("DM_DESC_DIAG");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/desc-diag");
    if(access(driver_option,F_OK)==0)setenv("DM_DESC_DIAG","1",1);
    unsetenv("DM_DISABLE_SET_RECYCLE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-set-recycle");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SET_RECYCLE","1",1);
    unsetenv("DM_DISABLE_RUNS_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-runs-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_RUNS_CACHE","1",1);
    unsetenv("DM_DISABLE_SHADER_GET_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-shader-get-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SHADER_GET_CACHE","1",1);
    unsetenv("DM_DISABLE_SHADER_STAMP");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-shader-stamp");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_SHADER_STAMP","1",1);
    unsetenv("DM_ACQUIRE_SYNC");
    marker_to_env("diagnostics/acquire-sync.txt","DM_ACQUIRE_SYNC",NULL);
    unsetenv("DM_FINALIZE_WAIT");
    marker_to_env("diagnostics/finalize-wait.txt","DM_FINALIZE_WAIT",NULL);
    unsetenv("DM_GUEST_WAIT_DIAG");
    marker_to_env("diagnostics/guest-wait-diag","DM_GUEST_WAIT_DIAG","1");
    unsetenv("DM_WSI_DIAG");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/wsi-diag");
    if(access(driver_option,F_OK)==0)setenv("DM_WSI_DIAG","1",1);
    unsetenv("DM_DRAW_TIMING");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/draw-timing");
    if(access(driver_option,F_OK)==0)setenv("DM_DRAW_TIMING","1",1);
    unsetenv("DM_PASS_STATS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pass-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_PASS_STATS","1",1);
    /* Diagnostic only: shader dump and texture LOD bias A/B. Absent markers leave both off. */
    /* Render submission slots in flight (1 = previous behaviour). */
    /* Present upscale filter: cubic (VK_EXT_filter_cubic) by default when the device has it; "linear" opts out. */
    unsetenv("DM_PRESENT_FILTER");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/present-filter.txt");
    {FILE *pf=fopen(driver_option,"r");if(pf){char pv[16]={0};size_t pn=fread(pv,1,15,pf);fclose(pf);while(pn&&(pv[pn-1]=='\n'||pv[pn-1]=='\r'||pv[pn-1]==' '))pv[--pn]=0;if(pn)setenv("DM_PRESENT_FILTER",pv,1);}}
    /* Attachment experiments are explicit. Never import the upstream defaults
     * or old coverage seed as evidence for this renderer/driver. */
    unsetenv("DM_ATT_LOAD_TABLE");unsetenv("DM_PASS_SENTINEL");unsetenv("DM_PASS_SENTINEL_SELFTEST");
    unsetenv("DM_ATT_STORE_SKIP");unsetenv("DM_RENDER_TRAFFIC");unsetenv("DM_COV_TABLE_CONTROL");
    unsetenv("DM_DISABLE_DYNAMIC_UBO");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-dynamic-ubo");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_DYNAMIC_UBO","1",1);
    unsetenv("DM_DIAG_COPY_VIEW");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-copy-view");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_COPY_VIEW","1",1);
    unsetenv("DM_DISABLE_GLOBAL_PASS_CACHE");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/disable-global-pass-cache");
    if(access(driver_option,F_OK)==0)setenv("DM_DISABLE_GLOBAL_PASS_CACHE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-att-load-table");
    if(access(driver_option,F_OK)==0)setenv("DM_ATT_LOAD_TABLE","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pass-sentinel");
    if(access(driver_option,F_OK)==0)setenv("DM_PASS_SENTINEL","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/pass-sentinel-selftest");
    if(access(driver_option,F_OK)==0){setenv("DM_PASS_SENTINEL","1",1);setenv("DM_PASS_SENTINEL_SELFTEST","1",1);}
    path(driver_option,sizeof driver_option,port_root,"diagnostics/enable-att-store-skip");
    if(access(driver_option,F_OK)==0)setenv("DM_ATT_STORE_SKIP","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/render-traffic");
    if(access(driver_option,F_OK)==0)setenv("DM_RENDER_TRAFFIC","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/cov-table.txt");
    setenv("DM_COV_TABLE_CONTROL",driver_option,1);
    unsetenv("DM_RESOLUTION_TIER");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/resolution-tier.txt");
    {FILE *f=fopen(driver_option,"r");if(f){int v=fgetc(f),tail=fgetc(f);fclose(f);
        if(v>='0'&&v<='2'&&(tail==EOF||tail=='\n')){char value[2]={(char)v,0};setenv("DM_RESOLUTION_TIER",value,1);}}}
    unsetenv("DM_DIAG_DUMP_FRAME");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/dump-frame.txt");
    {FILE *df=fopen(driver_option,"r");if(df){char dv[24]={0};size_t dn=fread(dv,1,23,df);fclose(df);if(dn)setenv("DM_DIAG_DUMP_FRAME",dv,1);}}
    unsetenv("DM_RENDER_SLOTS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/render-slots.txt");
    {FILE *rf=fopen(driver_option,"r");if(rf){char rv[8]={0};size_t rn=fread(rv,1,7,rf);fclose(rf);if(rn)setenv("DM_RENDER_SLOTS",rv,1);}}
    unsetenv("DM_ADAPTED_SHADER_DIR");unsetenv("DM_DIAG_LOD_BIAS");unsetenv("DM_SHADER_OVERRIDE_DIR");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/shader-override");
    if(access(driver_option,F_OK)==0)setenv("DM_SHADER_OVERRIDE_DIR",driver_option,1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/dump-shaders");
    if(access(driver_option,F_OK)==0){char dir[1024];path(dir,sizeof dir,port_root,"diagnostics/shaders");mkdir(dir,0700);setenv("DM_ADAPTED_SHADER_DIR",dir,1);}
    path(driver_option,sizeof driver_option,port_root,"diagnostics/lod-bias.txt");
    {FILE *lf=fopen(driver_option,"r");if(lf){char lv[16]={0};size_t ln=fread(lv,1,15,lf);fclose(lf);if(ln)setenv("DM_DIAG_LOD_BIAS",lv,1);}}
    /* Hint sessions are an explicit A/B experiment, off when the marker is absent. */
    unsetenv("DM_HINT_TARGET_NS");unsetenv("DM_TRANSLATE_AFFINITY");unsetenv("DM_ACQUIRE_AFFINITY");
    marker_to_env("diagnostics/hint-target-ns.txt","DM_HINT_TARGET_NS",NULL);
    marker_to_env("diagnostics/translate-affinity.txt","DM_TRANSLATE_AFFINITY",NULL);
    marker_to_env("diagnostics/acquire-affinity.txt","DM_ACQUIRE_AFFINITY",NULL);
    unsetenv("DM_PERFORMANCE_HINTS");
    path(driver_option,sizeof driver_option,port_root,"diagnostics/performance-hints");
    if(access(driver_option,F_OK)==0)setenv("DM_PERFORMANCE_HINTS","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/capture-frames");
    if(access(driver_option,F_OK)==0)setenv("DM_CAPTURE_FRAMES","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-barrier-cmd");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_BARRIER","1",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/diag-barrier-soft");
    if(access(driver_option,F_OK)==0)setenv("DM_DIAG_BARRIER","2",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/barrier-stats");
    if(access(driver_option,F_OK)==0)setenv("DM_BARRIER_STATS","1",1);
    char logs[PATH_MAX], log[PATH_MAX], shaders[PATH_MAX];
    path(logs,sizeof logs,port_root,"diagnostics");
    setenv("DM_FRAME_DIR",logs,1);
    path(driver_option,sizeof driver_option,logs,"attachment-mode.txt");
    setenv("DM_VK_ATTACHMENT_CONTROL",driver_option,1);
    /* Opt-in live A/B control. The file is sampled by the replay thread only
     * at frame boundaries; it does not restart or alter the game session. */
    unsetenv("DM_VK_AB_CONTROL");
    path(driver_option,sizeof driver_option,logs,"attachment-ab");
    if(access(driver_option,F_OK)==0){
        path(driver_option,sizeof driver_option,logs,"attachment-mode.txt");
        setenv("DM_VK_AB_CONTROL",driver_option,1);
    }
    if (dm_android_fs_ensure_directory(logs)) return errno?errno:EIO;
    char pipeline_dir[PATH_MAX];path(pipeline_dir,sizeof pipeline_dir,port_root,"pipeline-cache");
    if(dm_android_fs_ensure_directory(pipeline_dir))return errno?errno:EIO;
    setenv("DM_VK_PIPELINE_DIR",pipeline_dir,1);
    /* Launch-only diagnostic modes: 0 memory only, 1 disk, 2 disk + warm-up.
     * Unlike attachment controls, these need a restart to distinguish a fresh
     * process from ordinary in-process pipeline reuse. */
    setenv("DM_VK_PIPELINE_MODE","2",1);
    path(driver_option,sizeof driver_option,logs,"pipeline-mode.txt");
    FILE *pipeline_mode=fopen(driver_option,"r");
    if(pipeline_mode){int mode=-1;char extra;int fields=fscanf(pipeline_mode,"%d %c",&mode,&extra);fclose(pipeline_mode);
        if(fields!=1||mode<0||mode>2)return EINVAL;
        char value[2]={(char)('0'+mode),0};setenv("DM_VK_PIPELINE_MODE",value,1);}
    unsetenv("DM_VK_PIPELINE_TRACE");path(driver_option,sizeof driver_option,logs,"pipeline-ab");
    if(access(driver_option,F_OK)==0)setenv("DM_VK_PIPELINE_TRACE","1",1);
    path(log,sizeof log,logs,"game-startup.log");
    char previous_log[PATH_MAX];path(previous_log,sizeof previous_log,logs,"game-startup.previous.log");
    /* Preserve the failed session when a player relaunches after a crash.
     * rename replaces the prior generation atomically, without copying a live log. */
    if(rename(log,previous_log)&&errno!=ENOENT)return errno;
    char status_path[PATH_MAX]; path(status_path,sizeof status_path,logs,"status.json");
    if (unlink(status_path) && errno!=ENOENT) return errno;
    strcpy(report_file,status_path);path(report_temp,sizeof report_temp,logs,"status.native.new");
    int fd=open(log,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600); if (fd<0) return errno;
    if (dup2(fd,STDERR_FILENO)<0) {rc=errno;close(fd);return rc;} close(fd); setvbuf(stderr,NULL,_IONBF,0);
    g_t0_ns=now_ns(); g_flag.fs=1; g_flag.svc=1; g_flag.dbg=1;
    path(shaders,sizeof shaders,port_root,"shaders"); dm_vk_set_shader_root(shaders);
    LOG("ANDROID_HOST app_uid=%u min_api=29 original_modules=3 selected_addons=0 no_CPU_interpreter",getuid());
    return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeSurface(JNIEnv *env,jclass cls,jobject surface,jint width,jint height,jfloat density,jfloat hz,jint focus) {
    (void)cls;
    ANativeWindow *next=surface?ANativeWindow_fromSurface(env,surface):NULL;
    if (surface && !next) return EINVAL;
    if (focus<1 || focus>3) { if(next)ANativeWindow_release(next); return EINVAL; }
    /* This JNI runs on game-surface, never the UI thread. Keep the original
     * window rendering until the current background save has had its chance
     * to tick and Commit; a resume wakes this wait immediately. */
    pthread_mutex_lock(&state_lock);
    while (!shutdown_requested && atomic_load(&lifecycle_focus)==3 &&
           background_wait_epoch==lifecycle_epoch && background_done_epoch<background_wait_epoch)
        pthread_cond_wait(&lifecycle_cv,&state_lock);
    int shutdown=shutdown_requested;
    pthread_mutex_unlock(&state_lock);
    if (shutdown) { if(next)ANativeWindow_release(next); return 0; }
    /* Suspend before replacement; no guest can present through a released window. */
    if (atomic_load(&started)) dm_gfx_suspend(1);
    dm_vk_set_window(next);
    pthread_mutex_lock(&focus_lock);
    /* An old Surface job must not turn a foreground Activity background. */
    focus=surface_focus_locked(next!=NULL);
    atomic_store(&audio_background,focus!=1 || !next); android_audio_apply();
    pthread_mutex_lock(&state_lock);
    ANativeWindow *old=native_window; native_window=next;
    window_state=(dm_android_window_state){.focus=focus,.surface_available=next!=NULL,
        .width=(uint32_t)width,.height=(uint32_t)height,.density=density,.refresh_hz=hz,.thermal_status=-1};
    dm_android_window_state w=window_state;
    pthread_mutex_unlock(&state_lock);
    dm_android_platform_status status; dm_android_platform_get_status(&status);
    int window_error=status.bound?dm_android_platform_update_window(&w):0;
    pthread_mutex_unlock(&focus_lock);
    if (old) ANativeWindow_release(old);
    if (next && !atomic_load(&input_bound)) {
        dm_android_input_config input={.source="Android.SurfaceView.MotionEvent",.panel_width=1280,.panel_height=720,
            .source_ready=input_source_ready,.logical_npad_style=1,.logical_npad_id=0};
        int rc=dm_android_input_bind(&input);if(rc)return rc;atomic_store(&input_bound,1);
    }
    if (!next || focus!=1) dm_android_input_cancel();
    dm_vk_set_display_hz((int)(hz+0.5f));
    if (window_error) return window_error;
    if (next && atomic_load(&lifecycle_focus)==1) dm_gfx_suspend(0);
    return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeTouch(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled) {
    (void)cls;
    if (!atomic_load(&input_bound)) return ENODEV;
    if (cancelled) {dm_android_input_cancel_touch();return 0;}
    if (!ids || !xs || !ys) return EINVAL;
    jsize n=(*env)->GetArrayLength(env,ids);
    if(n>16 || (*env)->GetArrayLength(env,xs)!=n || (*env)->GetArrayLength(env,ys)!=n)return EINVAL;
    jint pointer_ids[16];jfloat px[16],py[16];dm_android_pointer points[16];
    (*env)->GetIntArrayRegion(env,ids,0,n,pointer_ids);(*env)->GetFloatArrayRegion(env,xs,0,n,px);(*env)->GetFloatArrayRegion(env,ys,0,n,py);
    if((*env)->ExceptionCheck(env))return EINVAL;
    for(jsize i=0;i<n;++i)points[i]=(dm_android_pointer){.id=(uint32_t)pointer_ids[i],.x=px[i],.y=py[i]};
    return dm_android_input_publish_touch(points,(size_t)n);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeStart(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;
    if (!atomic_load(&configured)) return ENODEV;
    pthread_mutex_lock(&focus_lock);
    pthread_mutex_lock(&state_lock);
    if (shutdown_requested || activity_focus!=1 || atomic_load(&lifecycle_focus)!=1 || !native_window) {
        pthread_mutex_unlock(&state_lock); pthread_mutex_unlock(&focus_lock); return EAGAIN;
    }
    int already=atomic_exchange(&started,1);
    pthread_mutex_unlock(&state_lock);
    if (already) { pthread_mutex_unlock(&focus_lock); return EALREADY; }
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,8*1024*1024);
    pthread_t worker;int error=pthread_create(&worker,&attr,game_thread,NULL);pthread_attr_destroy(&attr);
    if (error) atomic_store(&started,0); else pthread_detach(worker);
    pthread_mutex_unlock(&focus_lock); return error;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeActions(JNIEnv *env,jclass cls,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)cls;return dm_android_input_publish_actions((uint64_t)buttons,lx,ly,rx,ry);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeControllerButtons(JNIEnv *env,jclass cls,jlong buttons) {
    (void)env;(void)cls;return dm_android_input_publish_controller_buttons((uint64_t)buttons);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeControllerActions(JNIEnv *env,jclass cls,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)cls;return dm_android_input_publish_controller_actions((uint64_t)buttons,lx,ly,rx,ry);
}
JNIEXPORT jboolean JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeControllerMode(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return dm_android_input_controller_mode()!=0;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeTouchActivity(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;dm_android_input_touch_activity();
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeControllerCancel(JNIEnv *env,jclass cls,jlong buttons) {
    (void)env;(void)cls;(void)dm_android_input_cancel_controller((uint64_t)buttons);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeControllerCancelActions(JNIEnv *env,jclass cls,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)cls;(void)dm_android_input_cancel_controller_actions((uint64_t)buttons,lx,ly,rx,ry);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeAudioInterruption(JNIEnv *env,jclass cls,jboolean began) {
    (void)env; (void)cls;
    atomic_store(&audio_interrupted, began ? 1 : 0);
    android_audio_apply();
    LOG("Android lifecycle: audio focus %s", began ? "interrupted" : "resumed");
    return 0;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeShutdown(JNIEnv *env,jclass cls) {
    (void)env; (void)cls;
    pthread_mutex_lock(&focus_lock);
    pthread_mutex_lock(&state_lock);
    if (shutdown_requested) { pthread_mutex_unlock(&state_lock); pthread_mutex_unlock(&focus_lock); return; }
    shutdown_requested=1; ++lifecycle_epoch; background_pending_epoch=0;
    pthread_mutex_unlock(&state_lock);
    dm_android_save_cancel();
    pthread_mutex_lock(&state_lock);
    shutdown_save_epoch=dm_android_save_epoch(); shutdown_requested=2;
    pthread_cond_broadcast(&lifecycle_cv);
    pthread_mutex_unlock(&state_lock);
    pthread_once(&lifecycle_once,start_lifecycle_worker);
    if (lifecycle_worker_error) LOG("Android lifecycle: shutdown worker unavailable errno=%d",lifecycle_worker_error);
    pthread_mutex_unlock(&focus_lock);
}
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeStatus(JNIEnv *env,jclass cls) {
    (void)cls;char text[8192];
    status_text(text,sizeof text);
    persist_status();return (*env)->NewStringUTF(env,text);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeMapReveal(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;
#ifdef DM_ANDROID_D2R_UI
    return dm_android_map_request(NULL);
#else
    return ENOTSUP;
#endif
}
JNIEXPORT jlongArray JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeMapStatus(JNIEnv *env,jclass cls) {
    (void)cls;jlong values[8]={0};
#ifdef DM_ANDROID_D2R_UI
    dm_android_map_status s;dm_android_map_status_get(&s);
    values[0]=s.sequence;values[1]=s.state;values[2]=s.area;values[3]=s.total;
    values[4]=s.done;values[5]=s.ready;values[6]=s.error;
#endif
    jlongArray out=(*env)->NewLongArray(env,8);
    if(out)(*env)->SetLongArrayRegion(env,out,0,8,values);return out;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeRenderScale(JNIEnv *env,jclass cls,jfloat scale) {
    (void)env;(void)cls;
    return atomic_load(&configured)?dm_android_render_scale_request(scale):EAGAIN;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeRenderScaleAuto(
        JNIEnv *env,jclass cls) {
    (void)env;(void)cls;
    return atomic_load(&configured)?dm_android_render_scale_auto():EAGAIN;
}
