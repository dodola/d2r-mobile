#include "../host/dm.h"
#include "services_platform.h"
#include "services_sdk.h"
#include "services_fs.h"
#include "romfs_image.h"
#include "input_backend.h"
#include <jni.h>
#include <android/native_window_jni.h>
#include <stdatomic.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
extern void guest_thread_init(void);
extern void crash_thread_init(void);
extern uint32_t svc_thread_main_init(void);
extern int dm_android_diagnostics_flush(void);
extern void dm_vk_set_shader_root(const char *);
extern void dm_vk_set_window(void *);
extern void dm_vk_set_display_hz(int);
extern void dm_gfx_suspend(int);
extern void dm_audio_suspend(int);
extern void dm_android_real_resource_probe(const char *);

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *native_window;
static char app_files[PATH_MAX], port_root[PATH_MAX], locale_tag[32];
static char phase[96] = "等待游戏文件";
static _Atomic int configured, started, sdk_ready, main_entered, returned;
static _Atomic int input_route_ready, input_bound;
static _Atomic uint64_t presented_frames;
static dm_android_window_state window_state = {.focus=3, .thermal_status=-1};
static pthread_mutex_t report_lock = PTHREAD_MUTEX_INITIALIZER;
static char report_file[PATH_MAX], report_temp[PATH_MAX];
static void path(char *out, size_t cap, const char *base, const char *leaf) {
    if (snprintf(out, cap, "%s/%s", base, leaf) >= (int)cap) die("Android private path too long");
}
static void status_text(char *text,size_t capacity) {
    snprintf(text,capacity,"{\"phase\":\"%s\",\"sdk_ready\":%s,\"nnMain_entered\":%s,\"presented_frames\":%llu,\"returned\":%s}",phase,
        atomic_load(&sdk_ready)?"true":"false",atomic_load(&main_entered)?"true":"false",
        (unsigned long long)atomic_load(&presented_frames),atomic_load(&returned)?"true":"false");
}
static void persist_status(void) {
    if (!report_file[0]) return;
    pthread_mutex_lock(&report_lock);
    char text[384];pthread_mutex_lock(&state_lock);status_text(text,sizeof text);pthread_mutex_unlock(&state_lock);
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
static int pause_audio(int on) {dm_audio_suspend(on);return 0;}
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
    struct stat st;
    if (stat(image,&st) || st.st_size != 8973693207LL) die("Existing original RomFS image absent or wrong size");
    phase_set("正在读取游戏资源");
    int error = romfs_image_setup_checked(image,rom,8973693207ULL);
    if (error) die("RomFS image setup errno=%d",errno);
    if (dm_android_fs_ensure_directory(save) || dm_android_fs_ensure_directory(temp) || dm_android_fs_ensure_directory(profile)) die("Android private directories");
    setenv("DM_ROMFS",rom,1); setenv("DM_SAVE",save,1); setenv("DM_TEMP",temp,1);
    char private_vulkan[PATH_MAX];path(private_vulkan,sizeof private_vulkan,port_root,"diagnostics/private-vulkan");
    if(access(private_vulkan,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    dm_android_platform_config config = {.mode=DM_ANDROID_HOST_ACTIVITY,
        .profile_directory=profile, .locale=locale_tag, .locale_source="Activity.Configuration",
        /* Selected add-on set is explicitly empty. No unverified DLC is exposed. */
        .content_manifest_verified=1, .graphics_present=1, .audio_present=1,
        .suspend_and_drain_graphics=suspend_graphics, .pause_audio=pause_audio, .flush_diagnostics=dm_android_diagnostics_flush};
    if ((error=dm_android_platform_bind(&config)) || (error=dm_android_platform_register_services())) die("Android platform bind/register error=%d",error);
    pthread_mutex_lock(&state_lock); dm_android_window_state w=window_state; pthread_mutex_unlock(&state_lock);
    if ((error=dm_android_platform_update_window(&w))) die("Android initial window state error=%d",error);
    dm_android_service_register(DM_ANDROID_SERVICE_FS,dm_android_fs_initialize,NULL);
    phase_set("正在装载游戏");
    const char *names[]={"main","sdk","subsdk0"}; load_modules(modules,names,3); guest_thread_init();
    uint32_t handle=svc_thread_main_init();
    phase_set("正在初始化游戏"); call_original("_init_libc0");
    LOG("ANDROID_ORIGINAL_CALL nnosInitialize handle=%u",handle);
    ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(handle,0);
    LOG("ANDROID_ORIGINAL_RETURN nnosInitialize");
    call_original("_init_libc1"); call_original("nninitInitializeSdkModule");
    call_original("nninitStartup"); call_original("_init_libc2"); atomic_store(&sdk_ready,1);
    dm_android_real_resource_probe(port_root);
    phase_set("正在启动游戏"); run_module_inits();
    char *args[]={"D2RMobile",NULL};
    ((void (*)(int))required("_ZN2nn2os11SetHostArgcEi"))(1);
    ((void (*)(char **))required("_ZN2nn2os11SetHostArgvEPPc"))(args);
    atomic_store(&main_entered,1);persist_status();call_original("nnMain");
    atomic_store(&returned,1); phase_set("游戏已退出"); return NULL;
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
    char driver_option[PATH_MAX];path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan");
    if(access(driver_option,F_OK)==0)setenv("DM_PRIVATE_VULKAN","turnip",1);
    path(driver_option,sizeof driver_option,port_root,"diagnostics/private-vulkan-sysmem");
    if(access(driver_option,F_OK)==0){setenv("DM_PRIVATE_VULKAN","turnip",1);setenv("TU_DEBUG","sysmem",1);}
    path(driver_option,sizeof driver_option,port_root,"diagnostics/vulkan-validation");
    if(access(driver_option,F_OK)==0)setenv("DM_VULKAN_VALIDATION","1",1);
    char logs[PATH_MAX], log[PATH_MAX], shaders[PATH_MAX];
    path(logs,sizeof logs,port_root,"diagnostics");
    setenv("DM_FRAME_DIR",logs,1);
    if (dm_android_fs_ensure_directory(logs)) return errno?errno:EIO;
    path(log,sizeof log,logs,"game-startup.log");
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
    /* Suspend before replacement; no guest can present through a released window. */
    if (atomic_load(&started)) dm_gfx_suspend(1);
    dm_audio_suspend(focus!=1 || !next);
    dm_vk_set_window(next);
    pthread_mutex_lock(&state_lock);
    ANativeWindow *old=native_window; native_window=next;
    window_state=(dm_android_window_state){.focus=focus,.surface_available=next!=NULL,
        .width=(uint32_t)width,.height=(uint32_t)height,.density=density,.refresh_hz=hz,.thermal_status=-1};
    dm_android_window_state w=window_state;
    pthread_mutex_unlock(&state_lock);
    if (old) ANativeWindow_release(old);
    if (next && !atomic_load(&input_bound)) {
        dm_android_input_config input={.source="Android.SurfaceView.MotionEvent",.panel_width=1280,.panel_height=720,
            .source_ready=input_source_ready,.logical_npad_style=1,.logical_npad_id=0};
        int rc=dm_android_input_bind(&input);if(rc)return rc;atomic_store(&input_bound,1);
    }
    if (!next || focus!=1) dm_android_input_cancel();
    dm_vk_set_display_hz((int)(hz+0.5f));
    dm_android_platform_status status; dm_android_platform_get_status(&status);
    if (status.bound) {int rc=dm_android_platform_update_window(&w);if(rc)return rc;}
    if (next && focus==1) dm_gfx_suspend(0);
    return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeTouch(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled) {
    (void)cls;
    if (!atomic_load(&input_bound)) return ENODEV;
    if (cancelled) {dm_android_input_cancel();return 0;}
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
    if (!atomic_load(&configured) || !dm_android_native_window()) return ENODEV;
    if (atomic_exchange(&started,1)) return EALREADY;
    pthread_attr_t attr; pthread_attr_init(&attr); pthread_attr_setstacksize(&attr,8*1024*1024);
    pthread_t worker;int error=pthread_create(&worker,&attr,game_thread,NULL);pthread_attr_destroy(&attr);
    if (error) {atomic_store(&started,0);return error;} pthread_detach(worker);return 0;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeActions(JNIEnv *env,jclass cls,jlong buttons,jint lx,jint ly,jint rx,jint ry) {
    (void)env;(void)cls;return dm_android_input_publish_actions((uint64_t)buttons,lx,ly,rx,ry);
}
JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeStatus(JNIEnv *env,jclass cls) {
    (void)cls;char text[384];
    pthread_mutex_lock(&state_lock);
    status_text(text,sizeof text);
    pthread_mutex_unlock(&state_lock);persist_status();return (*env)->NewStringUTF(env,text);
}
