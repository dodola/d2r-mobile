/* Independent NativeActivity for the pinned Echoes 1.1.0 bring-up. No D2R
 * paths, instruction patches, app identity, or save directories are reused. */
#include "../host/hle.h"
#include "services_platform.h"
#include "services_fs.h"
#include "services_sdk.h"
#include "romfs_image.h"
#include "input_backend.h"
#include "audio_renderer_android.h"
#include "lifecycle_android.h"
#include "cloud_android.h"
#include "save_notice_android.h"
#include "title_probe_profile.h"
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/looper.h>
#include <android/choreographer.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
extern void guest_thread_init(void);
extern void crash_thread_init(void);
extern uint32_t svc_thread_main_init(void);
extern int dm_android_diagnostics_flush(void);
extern void dm_android_loader_probe_allow_unused_force(void);
extern void dm_android_svc_set_program_id(uint64_t);
extern void dm_vk_set_shader_root(const char *);
extern void dm_vk_set_window(void *);
extern void dm_gfx_suspend(int);
extern void dm_audio_suspend(int);
extern void dm_custom_init(const char *dir);
extern void dm_focus_set(int);
extern void dm_ui_lifecycle(int);

static ANativeWindow *window;
static pthread_mutex_t window_lock=PTHREAD_MUTEX_INITIALIZER;
static char private_root[PATH_MAX], content_root[PATH_MAX];
static _Atomic int launched;
static _Atomic int lifecycle_ready;
static _Atomic int lifecycle_destroyed;
static _Atomic int lifecycle_focus = 1;
static _Atomic int lifecycle_requested_focus = 1;
static _Atomic uint64_t last_resign_save_ns;
static float display_hz,display_density;
static pthread_mutex_t input_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t input_changed=PTHREAD_COND_INITIALIZER;
static AInputQueue *input_queue;
static ALooper *input_looper;
static JavaVM *input_vm;
static pthread_once_t input_once=PTHREAD_ONCE_INIT;
static _Atomic int input_route_available;
static int input_ready(void *unused) {(void)unused;return atomic_load(&input_route_available);}
static uint64_t button_bit(int key) {
    switch(key) {
        case AKEYCODE_BUTTON_A:case AKEYCODE_ENTER:return 1;
        case AKEYCODE_BUTTON_B:case AKEYCODE_SPACE:case AKEYCODE_BACK:return 2;
        case AKEYCODE_BUTTON_X:return 4;case AKEYCODE_BUTTON_Y:return 8;
        case AKEYCODE_BUTTON_THUMBL:return 16;case AKEYCODE_BUTTON_THUMBR:return 32;
        case AKEYCODE_BUTTON_L1:return 64;case AKEYCODE_BUTTON_R1:return 128;
        case AKEYCODE_BUTTON_L2:return 256;case AKEYCODE_BUTTON_R2:return 512;
        case AKEYCODE_BUTTON_START:return 1024;case AKEYCODE_BUTTON_SELECT:return 2048;
        case AKEYCODE_DPAD_LEFT:return 4096;case AKEYCODE_DPAD_UP:return 8192;
        case AKEYCODE_DPAD_RIGHT:return 16384;case AKEYCODE_DPAD_DOWN:return 32768;
        default:return 0;
    }
}
static int32_t stick(float value) {if(!isfinite(value))return 0;if(value>1)value=1;if(value<-1)value=-1;return (int32_t)(value*32767);}
typedef struct { int used,id; uint64_t keys,motion; int32_t lx,ly,rx,ry; int ranges_known,right_x,right_y; float flat[4]; } controller_device;
static controller_device *controller_slot(controller_device *devices,int id) {
    controller_device *empty=NULL;
    for(int i=0;i<16;i++){if(devices[i].used&&devices[i].id==id)return devices+i;if(!devices[i].used&&!empty)empty=devices+i;}
    if(empty){empty->used=1;empty->id=id;}return empty;
}
static void controller_publish(const controller_device *devices,uint64_t reset_generation) {
    uint64_t buttons=0;int32_t lx=0,ly=0,rx=0,ry=0;int64_t left=-1,right=-1;
    for(int i=0;i<16;i++)if(devices[i].used){const controller_device *d=devices+i;buttons|=d->keys|d->motion;
        int64_t l=(int64_t)d->lx*d->lx+(int64_t)d->ly*d->ly,r=(int64_t)d->rx*d->rx+(int64_t)d->ry*d->ry;
        if(l>left){left=l;lx=d->lx;ly=d->ly;}if(r>right){right=r;rx=d->rx;ry=d->ry;}}
    dm_android_input_publish_controller_actions_at_generation(buttons,lx,ly,rx,ry,reset_generation);
}
static void controller_ranges(JNIEnv *env,jclass device_class,jmethodID get_device,
                              jmethodID get_range,jmethodID get_flat,controller_device *d,int source) {
    if(d->ranges_known)return;
    d->right_x=AMOTION_EVENT_AXIS_Z;d->right_y=AMOTION_EVENT_AXIS_RZ;
    jobject device=(*env)->CallStaticObjectMethod(env,device_class,get_device,d->id);
    if(!device || (*env)->ExceptionCheck(env)){if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);if(device)(*env)->DeleteLocalRef(env,device);return;}
    int axes[4]={AMOTION_EVENT_AXIS_X,AMOTION_EVENT_AXIS_Y,AMOTION_EVENT_AXIS_Z,AMOTION_EVENT_AXIS_RZ};
    float flat[4]={0};int failed=0;
    for(int i=0;i<4;i++){
        jobject range=(*env)->CallObjectMethod(env,device,get_range,axes[i],source);
        if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);if(range)(*env)->DeleteLocalRef(env,range);failed=1;break;}
        if(!range && i>=2){axes[i]=i==2?AMOTION_EVENT_AXIS_RX:AMOTION_EVENT_AXIS_RY;range=(*env)->CallObjectMethod(env,device,get_range,axes[i],source);
            if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);if(range)(*env)->DeleteLocalRef(env,range);failed=1;break;}}
        if(range){flat[i]=(*env)->CallFloatMethod(env,range,get_flat);(*env)->DeleteLocalRef(env,range);
            if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);failed=1;break;}}
    }
    if(!failed){memcpy(d->flat,flat,sizeof flat);d->right_x=axes[2];d->right_y=axes[3];d->ranges_known=1;}
    (*env)->DeleteLocalRef(env,device);
}
static int32_t controller_axis(AInputEvent *event,int axis,float flat,int invert) {
    float value=AMotionEvent_getAxisValue(event,axis,0);if(fabsf(value)<=flat)value=0;
    return stick(invert?-value:value);
}
/* NativeActivity does not have a Java View to forward MotionEvent objects to.
 * Keep the original touchscreen contract alive at the AInputQueue boundary:
 * coordinates are normalized against the actual Surface and pointer-up events
 * omit only the pointer that went away.  This is deliberately title-agnostic;
 * Echoes receives the same nn::hid touch states as any other title and no
 * private game/UI address is needed. */
static int touch_motion(AInputEvent *event,uint64_t reset_generation) {
    int action=AMotionEvent_getAction(event);
    int masked=action & AMOTION_EVENT_ACTION_MASK;
    if(masked==AMOTION_EVENT_ACTION_CANCEL) {
        dm_android_input_cancel_touch();
        return 1;
    }
    if(masked!=AMOTION_EVENT_ACTION_DOWN && masked!=AMOTION_EVENT_ACTION_POINTER_DOWN &&
       masked!=AMOTION_EVENT_ACTION_MOVE && masked!=AMOTION_EVENT_ACTION_POINTER_UP &&
       masked!=AMOTION_EVENT_ACTION_UP) return 0;
    pthread_mutex_lock(&window_lock);
    ANativeWindow *w=window;
    int width=w?ANativeWindow_getWidth(w):0, height=w?ANativeWindow_getHeight(w):0;
    pthread_mutex_unlock(&window_lock);
    if(width<=0 || height<=0) return 0;
    size_t count=(size_t)AMotionEvent_getPointerCount(event);
    int lifted=(masked==AMOTION_EVENT_ACTION_POINTER_UP)?
        ((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT):-1;
    if(masked==AMOTION_EVENT_ACTION_UP) count=0;
    dm_android_pointer points[16]; size_t out=0;
    for(size_t i=0;i<count && i<16;i++) {
        if((int)i==lifted) continue;
        float x=AMotionEvent_getX(event,(size_t)i), y=AMotionEvent_getY(event,(size_t)i);
        uint32_t id=(uint32_t)AMotionEvent_getPointerId(event,(size_t)i);
        points[out++]=(dm_android_pointer){.id=id,.x=x/(float)width,.y=y/(float)height};
    }
    int rc=dm_android_input_publish_touch_at_generation(points,out,reset_generation);
    if(rc && rc!=ENODEV && rc!=EAGAIN) {
        static _Atomic unsigned failures;
        unsigned n=atomic_fetch_add(&failures,1);
        if(n<8) LOG("ECHOES_TOUCH_PUBLISH_FAILED action=%d pointers=%zu errno=%d",masked,out,rc);
    }
    return 1;
}
static void *input_worker(void *unused) {
    (void)unused;controller_device devices[16]={{0}};uint64_t reset_generation=0;
    /* NativeActivity has no InputDeviceListener. Poll the platform's current
     * device IDs so unplugging one controller cannot leave its keys/sticks
     * held, and so another controller's contribution survives the removal. */
    JNIEnv *env=NULL;
    if(!input_vm || (*input_vm)->AttachCurrentThread(input_vm,&env,NULL)!=JNI_OK)die("Echoes input JNI attach");
    jclass device_class=(*env)->FindClass(env,"android/view/InputDevice");
    if(!device_class || (*env)->ExceptionCheck(env))die("Echoes InputDevice class unavailable");
    jmethodID device_ids=device_class?(*env)->GetStaticMethodID(env,device_class,"getDeviceIds","()[I"):NULL;
    if(!device_ids || (*env)->ExceptionCheck(env))die("Echoes InputDevice enumeration unavailable");
    jmethodID get_device=device_class?(*env)->GetStaticMethodID(env,device_class,"getDevice","(I)Landroid/view/InputDevice;"):NULL;
    if(!get_device || (*env)->ExceptionCheck(env))die("Echoes InputDevice lookup unavailable");
    jmethodID get_range=device_class?(*env)->GetMethodID(env,device_class,"getMotionRange","(II)Landroid/view/InputDevice$MotionRange;"):NULL;
    if(!get_range || (*env)->ExceptionCheck(env))die("Echoes InputDevice ranges unavailable");
    jclass range_class=(*env)->FindClass(env,"android/view/InputDevice$MotionRange");
    if(!range_class || (*env)->ExceptionCheck(env))die("Echoes InputDevice range class unavailable");
    jmethodID get_flat=range_class?(*env)->GetMethodID(env,range_class,"getFlat","()F"):NULL;
    if(!device_ids || !get_device || !get_range || !get_flat || (*env)->ExceptionCheck(env))die("Echoes InputDevice enumeration unavailable");
    pthread_mutex_lock(&input_lock);input_looper=ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    if(input_queue)AInputQueue_attachLooper(input_queue,input_looper,1,NULL,NULL);
    pthread_mutex_unlock(&input_lock);
    for(;;) {
        (void)ALooper_pollOnce(100,NULL,NULL,NULL);
        pthread_mutex_lock(&input_lock);AInputEvent *event;
        dm_android_input_status input_status;dm_android_input_status_get(&input_status);
        if(reset_generation!=input_status.input_resets){memset(devices,0,sizeof devices);reset_generation=input_status.input_resets;}
        while(input_queue && AInputQueue_getEvent(input_queue,&event)>=0) {
            if(AInputQueue_preDispatchEvent(input_queue,event))continue;
            if(atomic_load(&lifecycle_focus)!=1){AInputQueue_finishEvent(input_queue,event,1);continue;}
            int handled=0,controller_event=0,type=AInputEvent_getType(event);
            if(type==AINPUT_EVENT_TYPE_KEY) {
                uint64_t bit=button_bit(AKeyEvent_getKeyCode(event));
                controller_device *d=controller_slot(devices,AInputEvent_getDeviceId(event));
                if(bit&&d){if(AKeyEvent_getAction(event)==AKEY_EVENT_ACTION_DOWN)d->keys|=bit;else d->keys&=~bit;handled=controller_event=1;}
            } else if(type==AINPUT_EVENT_TYPE_MOTION && (AInputEvent_getSource(event)&AINPUT_SOURCE_JOYSTICK)==AINPUT_SOURCE_JOYSTICK) {
                controller_device *d=controller_slot(devices,AInputEvent_getDeviceId(event));
                if(d){controller_ranges(env,device_class,get_device,get_range,get_flat,d,AInputEvent_getSource(event));
                    d->lx=controller_axis(event,AMOTION_EVENT_AXIS_X,d->flat[0],0);d->ly=controller_axis(event,AMOTION_EVENT_AXIS_Y,d->flat[1],1);
                    d->rx=controller_axis(event,d->right_x,d->flat[2],0);d->ry=controller_axis(event,d->right_y,d->flat[3],1);
                    float hx=AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_HAT_X,0),hy=AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_HAT_Y,0);d->motion=0;
                    if(hx<-.5f)d->motion|=4096;if(hx>.5f)d->motion|=16384;if(hy<-.5f)d->motion|=8192;if(hy>.5f)d->motion|=32768;
                    if(fmaxf(AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_LTRIGGER,0),AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_BRAKE,0))>.5f)d->motion|=256;
                    if(fmaxf(AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_RTRIGGER,0),AMotionEvent_getAxisValue(event,AMOTION_EVENT_AXIS_GAS,0))>.5f)d->motion|=512;
                    handled=controller_event=1;}
            } else if(type==AINPUT_EVENT_TYPE_MOTION &&
                      (AInputEvent_getSource(event)&AINPUT_SOURCE_TOUCHSCREEN)==AINPUT_SOURCE_TOUCHSCREEN &&
                      atomic_load(&launched)) {
                handled=touch_motion(event,reset_generation);
            }
            if(controller_event && atomic_load(&launched))controller_publish(devices,reset_generation);
            AInputQueue_finishEvent(input_queue,event,handled);
        }
        int removed=0;
        jintArray current_ids=(*env)->CallStaticObjectMethod(env,device_class,device_ids);
        if(!(*env)->ExceptionCheck(env) && current_ids){
            jsize count=(*env)->GetArrayLength(env,current_ids);
            jint *ids=(*env)->ExceptionCheck(env)?NULL:(*env)->GetIntArrayElements(env,current_ids,NULL);
            if(ids){for(int slot=0;slot<16;slot++)if(devices[slot].used && devices[slot].id>=0){int present=0;
                for(jsize i=0;i<count;i++)present|=devices[slot].id==ids[i];
                if(!present){memset(devices+slot,0,sizeof devices[slot]);removed=1;}}
                (*env)->ReleaseIntArrayElements(env,current_ids,ids,JNI_ABORT);}
        }else if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
        if((*env)->ExceptionCheck(env))(*env)->ExceptionClear(env);
        if(current_ids)(*env)->DeleteLocalRef(env,current_ids);
        if(removed && atomic_load(&launched) && atomic_load(&lifecycle_focus)==1)controller_publish(devices,reset_generation);
        pthread_mutex_unlock(&input_lock);
    }
    return NULL;
}
static void start_input(void) {pthread_t thread;if(pthread_create(&thread,NULL,input_worker,NULL))die("Echoes input thread");pthread_detach(thread);}
static void input_created(ANativeActivity *activity,AInputQueue *queue) {
    (void)activity;pthread_mutex_lock(&input_lock);input_queue=queue;atomic_store(&input_route_available,1);
    if(input_looper)AInputQueue_attachLooper(queue,input_looper,1,NULL,NULL);
    pthread_cond_broadcast(&input_changed);pthread_mutex_unlock(&input_lock);pthread_once(&input_once,start_input);
    dm_android_input_style_changed();
}
static void input_destroyed(ANativeActivity *activity,AInputQueue *queue) {
    (void)activity;pthread_mutex_lock(&input_lock);AInputQueue_detachLooper(queue);input_queue=NULL;atomic_store(&input_route_available,0);
    dm_android_input_cancel();pthread_mutex_unlock(&input_lock);
    dm_android_input_style_changed();
}
static pthread_mutex_t exit_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned exit_sections;
static int exit_pending;
static _Atomic int exit_save_started;
static const char *exit_save_reason;
static pthread_mutex_t vsync_lock=PTHREAD_MUTEX_INITIALIZER;
static void *vsync_events[32];
static unsigned vsync_event_count;
static _Atomic int vsync_active;
static AChoreographer *choreographer;
extern void hle_bind_system_event(void *,int);
extern void hle_signal_system_event(void *);
static void vsync_frame(int64_t time,void *unused) {
    (void)time;if(!atomic_load(&vsync_active))return;
    if (atomic_load(&lifecycle_focus)==1) dm_android_save_notice_poll(unused);
    pthread_mutex_lock(&vsync_lock);
    for(unsigned i=0;i<vsync_event_count;i++)hle_signal_system_event(vsync_events[i]);
    pthread_mutex_unlock(&vsync_lock);
    AChoreographer_postFrameCallback64(choreographer,vsync_frame,unused);
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "Original SDK SystemEvent signalled by Android Choreographer frames", Result, echoes_vsync_event, void *event,void *display) {
    int dm_android_vi_display_valid(const void *);
    if(!event || !dm_android_vi_display_valid(display))return RES_FAIL;
    hle_bind_system_event(event,1);
    pthread_mutex_lock(&vsync_lock);
    for(unsigned i=0;i<vsync_event_count;i++)if(vsync_events[i]==event){pthread_mutex_unlock(&vsync_lock);return RES_OK;}
    if(vsync_event_count==32)die("Echoes display event capacity");
    vsync_events[vsync_event_count++]=event;pthread_mutex_unlock(&vsync_lock);return RES_OK;
}
static _Atomic int joy_hold_type;
static _Atomic int requested_cpu_boost;
static _Atomic int startup_logo_finished;
HLE_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "Android NativeActivity has no Horizon startup-logo layer to dismiss", void, echoes_finish_startup_logo,void) {
    atomic_store(&startup_logo_finished,1);LOG("ECHOES_STARTUP_LOGO_FINISHED surface=NativeActivity");
}
HLE_FORCE("_ZN2nn6applet15SetCpuBoostModeEi", "Keep title boost preference; Android owns CPU frequency policy", void, echoes_cpu_boost,int mode) {
    if(mode<0 || mode>1)die("Echoes unsupported CPU boost mode=%d",mode);
    atomic_store(&requested_cpu_boost,mode);LOG("ECHOES_CPU_BOOST requested=%d policy=android-governor",mode);
}
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEj", "Android file access is logged by DM_FSLOG; Horizon fsp-srv logging is disabled", int, echoes_sdk_fs_access_log,unsigned mode) {
    (void)mode;return 0;
}
static pthread_once_t font_once=PTHREAD_ONCE_INIT;
static struct {const void *address;size_t size;} shared_fonts[6];
static void load_shared_fonts(void) {
    for(unsigned i=0;i<6;i++) {
        char path[PATH_MAX];snprintf(path,sizeof(path),"%s/fonts/%u.sfnt",content_root,i);
        int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);struct stat st;
        if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<12 || st.st_size>32*1024*1024)
            die("Echoes shared font missing/invalid type=%u path=%s",i,path);
        const uint8_t *data=mmap(NULL,st.st_size,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
        if(data==MAP_FAILED)die("Echoes shared font mapping type=%u errno=%d",i,errno);
        if(memcmp(data,"\x00\x01\x00\x00",4)&&memcmp(data,"OTTO",4))die("Echoes shared font SFNT signature type=%u",i);
        unsigned tables=(unsigned)data[4]*256+data[5];
        if(!tables || tables>512 || 12u+16u*tables>(uint64_t)st.st_size)die("Echoes shared font table directory type=%u",i);
        for(unsigned j=0;j<tables;j++) {
            const uint8_t *entry=data+12+16*j;uint32_t offset=0,size=0;
            for(unsigned k=0;k<4;k++){offset=offset*256+entry[8+k];size=size*256+entry[12+k];}
            if(offset>(uint64_t)st.st_size || size>(uint64_t)st.st_size-offset)die("Echoes shared font table bounds type=%u",i);
        }
        shared_fonts[i].address=data;shared_fonts[i].size=st.st_size;
        LOG("ECHOES_SHARED_FONT type=%u bytes=%zu",i,shared_fonts[i].size);
    }
}
static void check_shared_font(int type) {
    if(type<0 || type>=6)die("Echoes shared font type=%d",type);
    if(pthread_once(&font_once,load_shared_fonts))die("Echoes shared font initialization");
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", "Read real local firmware SFNT resources instead of Horizon pl:u IPC", int, echoes_font_state, int type) {
    check_shared_font(type);return 1;
}
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", "Keep mapped local firmware font storage alive for the title lifetime", const void *, echoes_font_address, int type) {
    check_shared_font(type);return shared_fonts[type].address;
}
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", "Report the actual validated local firmware SFNT byte length", size_t, echoes_font_size, int type) {
    check_shared_font(type);return shared_fonts[type].size;
}
HLE_FORCE("fopen", "Keep SDK FILE ABI and read Android mounted resources through guest fopencookie", void *, echoes_fopen, const char *path,const char *mode) {
    void *dm_android_fs_open_guest_cookie(const char *,const char *,void *);
    void *create=guest_sym("fopencookie");if(!create)die("Echoes SDK fopencookie missing");
    return dm_android_fs_open_guest_cookie(path,mode,create);
}
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", "Android exact-length ReadFile overload uses the same verified RomFS path", Result, echoes_read_exact, uint64_t handle,int64_t offset,void *buffer,size_t size) {
    Result dm_android_fs_read_exact(uint64_t,int64_t,void *,size_t);
    return dm_android_fs_read_exact(handle,offset,buffer,size);
}
HLE_FORCE("_ZN2nn2vi12SetLayerCropEPNS0_5LayerEiiii", "Apply VI layer crop to the Android Vulkan presentation blit", Result, echoes_layer_crop, void *layer,int x,int y,int width,int height) {
    Result dm_android_vi_set_layer_crop(void *,int,int,int,int);
    return dm_android_vi_set_layer_crop(layer,x,y,width,height);
}
HLE_FORCE("_ZN2nn3hid28SetNpadJoyAssignmentModeDualERKj", "Store Joy-Con pairing preference; Android logical FullKey remains separate", void, echoes_joy_dual, const uint32_t *id) {
    if(!id || (*id>7 && *id!=0x10 && *id!=0x20))die("Echoes invalid pairing ID");
    char name[48];snprintf(name,sizeof name,"joy_dual_preference_%u",*id);dm_android_platform_setting(name,1);
}
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Store requested Joy-Con orientation independently of logical FullKey input", void, echoes_set_joy_hold, int type) {
    if(type!=0 && type!=1)die("Echoes invalid Joy-Con hold type");atomic_store(&joy_hold_type,type);
}
HLE_FORCE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", "Read the Android title's requested Joy-Con orientation", int, echoes_get_joy_hold, void) {return atomic_load(&joy_hold_type);}
extern void dm_host_shutdown(const char *);
extern void dm_android_oe_request_exit(void);
extern void *dm_android_oe_notification_event(void);
extern uint64_t dm_fs_commit_count(void);

/* NativeActivity destruction can race the guest's notification pump. Queue
 * the original applet exit request from a helper thread, then wait for the
 * real nn::fs::Commit hook before draining Android backends. Waiting on the
 * Activity callback itself would block the guest thread on some framework
 * versions and make the queued request impossible to consume. */
static void *exit_save_worker(void *unused) {
    (void)unused;
    uint64_t before=dm_fs_commit_count(),commits=0; double elapsed=0;
    dm_android_oe_request_exit();
    int observed=dm_android_save_wait_for_commit(2.5,before,&commits,&elapsed);
    LOG("ECHOES_EXIT_SAVE_WAIT requested=1 observed_commits=%llu wait_result=%d elapsed_ms=%.0f",(unsigned long long)commits,observed,elapsed);
    dm_android_cloud_flush_wait(3.0);
    dm_android_cloud_shutdown();
    dm_host_shutdown(exit_save_reason?exit_save_reason:"Echoes Activity destroyed");
    return NULL;
}
static void request_exit_save(void) {
    if(atomic_exchange(&exit_save_started,1))return;
    pthread_t thread;
    if(pthread_create(&thread,NULL,exit_save_worker,NULL)) {
        LOG("ECHOES_EXIT_SAVE_WORKER_FAILED errno=%d; shutting down without bounded save wait",errno);
        dm_host_shutdown(exit_save_reason?exit_save_reason:"Echoes Activity destroyed");
        return;
    }
    pthread_detach(thread);
}

/* NativeActivity exposes the same four scene transitions as iOS, but through
 * onPause/onStop/window-focus callbacks. Android commonly reports focus loss
 * immediately before onPause and repeats stop during surface replacement, so
 * the transition is idempotent. Save work uses only the reviewed title-adapter
 * ABI; without one we still sync the transactional tree and never guess an
 * Echoes private address. */
static void echoes_lifecycle_event(int event) {
    int focus = event == 0 ? 2 : event == 1 ? 3 : event == 2 ? 2 : event == 3 ? 1 : -1;
    if (focus < 0) return;
    atomic_store(&lifecycle_requested_focus, focus);
    if (!atomic_load(&lifecycle_ready) || atomic_load(&lifecycle_destroyed)) return;
    int previous = atomic_exchange(&lifecycle_focus, focus);
    if (previous == focus) return;
    if (focus != 1) {
        /* Releases may be delivered while the window is already losing focus;
         * clear all input at the same boundary as iOS resign-active. */
        dm_android_input_cancel();
        dm_focus_set(focus == 3 ? 3 : 2);
        if (event == 0) {
            uint64_t now = now_ns(), last = atomic_load(&last_resign_save_ns);
            if (dm_android_save_adapter_available() && dm_ingame_now() &&
                (!last || now - last >= 30000000000ull) && dm_request_save()) {
                atomic_store(&last_resign_save_ns, now);
                LOG("ECHOES_LIFECYCLE resign-active save queued");
            }
        }
        if (focus == 3) {
            dm_ui_lifecycle(1);
            dm_audio_suspend(1); dm_android_ar_suspend(1);
            uint64_t commits = 0, before = dm_fs_commit_count();
            int froze = 0; double elapsed = 0;
            if (dm_android_save_adapter_available() && dm_ingame_now()) {
                dm_save_wait(2.5, &commits, &froze, &elapsed);
                LOG("ECHOES_LIFECYCLE background save wait commits=%llu froze=%d elapsed_ms=%.0f",
                    (unsigned long long)commits, froze, elapsed);
            } else {
                LOG("ECHOES_LIFECYCLE background save adapter unavailable; waiting only for existing commits");
                dm_android_save_wait_for_commit(0.25, before, &commits, &elapsed);
            }
            const char *save = getenv("DM_SAVE");
            if (save && dm_android_fs_sync_tree(save))
                LOG("ECHOES_LIFECYCLE save tree sync failed errno=%d", errno);
            /* The save commit hook may enqueue its snapshot just after the
             * commit counter changes; wait after the bounded save wait so the
             * newest snapshot is included in the cloud flush. */
            dm_android_cloud_flush_wait(3.0);
            guest_clock_freeze_for(1, 1);
            dm_gfx_suspend(1);
        }
        return;
    }
    dm_ui_lifecycle(0);
    dm_android_cloud_foreground();
    guest_clock_freeze_for(1, 0);
    dm_gfx_suspend(0);
    dm_audio_suspend(0); dm_android_ar_suspend(0);
    dm_focus_set(1);
}

static void activity_on_start(ANativeActivity *activity) { (void)activity; echoes_lifecycle_event(2); }
static void activity_on_resume(ANativeActivity *activity) { (void)activity; echoes_lifecycle_event(2); }
static void activity_on_pause(ANativeActivity *activity) {
    (void)activity; echoes_lifecycle_event(0); echoes_lifecycle_event(1);
}
static void activity_on_stop(ANativeActivity *activity) { (void)activity; echoes_lifecycle_event(1); }
static void activity_focus_changed(ANativeActivity *activity, int focused) {
    (void)activity; echoes_lifecycle_event(focused ? 3 : 0);
}
HLE_FORCE("_ZN2nn2oe27GetNotificationMessageEventEv", "Android notification queue backed by original SDK SystemEvent", void *, echoes_notification_event, void) {
    return dm_android_oe_notification_event();
}
HLE_FORCE("_ZN2nn3hid27BindNpadStyleSetUpdateEventERKjPNS_2os15SystemEventTypeENS3_14EventClearModeE", "Original SDK event reflects Android logical input style changes", void, echoes_style_event, const uint32_t *id,void *event,int mode) {
    int rc=id?dm_android_input_bind_style_event(*id,event,mode):EINVAL;
    if(rc)die("Echoes input style event=%d",rc);
}
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "Android title exit deferral nesting", void, echoes_enter_exit, void) {
    pthread_mutex_lock(&exit_lock);
    if(exit_sections==UINT_MAX)die("Echoes exit handling nesting overflow");
    exit_sections++;
    pthread_mutex_unlock(&exit_lock);
}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "Android title exit deferral nesting", void, echoes_leave_exit, void) {
    pthread_mutex_lock(&exit_lock);
    if(!exit_sections)die("Echoes unbalanced exit handling section");
    int stop=!--exit_sections && exit_pending;
    pthread_mutex_unlock(&exit_lock);
    /* The exit-save worker owns the bounded wait and shutdown. This callback
     * runs on the guest thread; waiting here would prevent the queued exit
     * notification from reaching the title. */
    if(stop)LOG("ECHOES_EXIT_SECTION_RELEASED pending=%d",exit_pending);
}
static void path(char *out,size_t cap,const char *base,const char *leaf) {
    if(snprintf(out,cap,"%s/%s",base,leaf)>=(int)cap)die("Echoes path length");
}
static void status(const char *stage,uint64_t frame) {
    char name[PATH_MAX];path(name,sizeof name,private_root,"bringup-status.json");
    FILE *f=fopen(name,"w");if(!f)die("Echoes status file");
    fprintf(f,"{\"title\":\"echoes-of-wisdom-1.1.0\",\"stage\":\"%s\",\"presented_frames\":%llu,\"full_game_verified\":false}\n",stage,(unsigned long long)frame);
    if(fclose(f))die("Echoes status close");
    LOG("ECHOES_STAGE %s frame=%llu",stage,(unsigned long long)frame);
}
void *dm_android_native_window(void) {
    pthread_mutex_lock(&window_lock);void *w=window;pthread_mutex_unlock(&window_lock);return w;
}
void dm_android_front_presented(uint64_t frame) {
    if(frame==1 || frame%300==0)status("presented",frame);
}
static int suspend_graphics(int on) {dm_gfx_suspend(on);return 0;}
static int pause_audio(int on) {dm_audio_suspend(on);dm_android_ar_suspend(on);return 0;}
static void *required(const char *name) {
    void *fn=guest_sym(name);if(!fn)die("Echoes missing startup export %s",name);return fn;
}
static void original(const char *name) {
    LOG("ECHOES_CALL %s",name);((void (*)(void))required(name))();LOG("ECHOES_RETURN %s",name);
}
static void *game_thread(void *unused) {
    (void)unused;crash_thread_init();LOG("ECHOES_GAME_THREAD tid=%d",gettid());
    char modules[PATH_MAX],image[PATH_MAX],rom[PATH_MAX],save[PATH_MAX],temp[PATH_MAX],profile[PATH_MAX];
    path(modules,sizeof modules,content_root,"modules");path(image,sizeof image,content_root,"game.romfs");
    path(rom,sizeof rom,private_root,"romfs-view");path(save,sizeof save,private_root,"save");
    path(temp,sizeof temp,private_root,"temp");path(profile,sizeof profile,private_root,"profile");
    status("resources",0);
    if(romfs_image_setup_checked(image,rom,8036390636ULL))die("Echoes RomFS setup errno=%d",errno);
    if(dm_android_fs_ensure_directory(save)||dm_android_fs_ensure_directory(temp)||dm_android_fs_ensure_directory(profile))die("Echoes directories");
    setenv("DM_ROMFS",rom,1);setenv("DM_SAVE",save,1);setenv("DM_TEMP",temp,1);
    /* Custom settings live beside the private save tree and are initialized
     * before the guest mounts save, as on iOS. */
    dm_custom_init(private_root);
    int rc=dm_android_cloud_boot(save);
    if(rc) LOG("Echoes Android cloud startup skipped/error=%d",rc);
    dm_android_platform_config config={.mode=DM_ANDROID_HOST_ACTIVITY,.profile_directory=profile,
        .content_manifest_verified=1,.graphics_present=1,.audio_present=1,
        .suspend_and_drain_graphics=suspend_graphics,.pause_audio=pause_audio,.flush_diagnostics=dm_android_diagnostics_flush};
    rc=dm_android_platform_bind(&config);if(rc)die("Echoes platform bind=%d",rc);
    if((rc=dm_android_platform_register_services()))die("Echoes platform services=%d",rc);
    dm_android_service_register(DM_ANDROID_SERVICE_FS,dm_android_fs_initialize,NULL);
    pthread_mutex_lock(&window_lock);
    dm_android_window_state state={.focus=1,.surface_available=window!=NULL,.width=ANativeWindow_getWidth(window),
        .height=ANativeWindow_getHeight(window),.density=display_density,.refresh_hz=display_hz,.thermal_status=-1,.generation=1};
    pthread_mutex_unlock(&window_lock);
    if((rc=dm_android_platform_update_window(&state)))die("Echoes window state=%d",rc);
    atomic_store(&lifecycle_ready,1);
    if (atomic_load(&lifecycle_requested_focus) != 1) echoes_lifecycle_event(
        atomic_load(&lifecycle_requested_focus) == 3 ? 1 : 0);
    pthread_mutex_lock(&input_lock);while(!input_queue)pthread_cond_wait(&input_changed,&input_lock);pthread_mutex_unlock(&input_lock);
    dm_android_input_config input={.source="NativeActivity.AInputQueue gamepad/keyboard",.panel_width=1280,.panel_height=720,
        .source_ready=input_ready,.logical_npad_id=0,.logical_npad_style=1};
    if((rc=dm_android_input_bind(&input)))die("Echoes input bind=%d",rc);
    enum {count=sizeof echoes_probe_modules/sizeof *echoes_probe_modules};
    const char *names[count];for(unsigned i=0;i<count;i++)names[i]=echoes_probe_modules[i].name;
    dm_android_loader_probe_allow_unused_force();dm_android_svc_set_program_id(echoes_probe_program_id);
    status("load",0);load_modules(modules,names,count);guest_thread_init();
    uint32_t handle=svc_thread_main_init();original("_init_libc0");
    LOG("ECHOES_CALL nnosInitialize handle=%u",handle);
    ((void (*)(uint64_t,uint64_t))required("nnosInitialize"))(handle,0);
    original("_init_libc1");original("nninitInitializeSdkModule");original("nninitStartup");original("_init_libc2");
    status("constructors",0);run_module_inits();
    char *args[]={"EchoesOfWisdom",NULL};
    ((void (*)(int))required("_ZN2nn2os11SetHostArgcEi"))(1);
    ((void (*)(char **))required("_ZN2nn2os11SetHostArgvEPPc"))(args);
    status("nnMain",0);
    char hold[PATH_MAX];path(hold,sizeof hold,private_root,"debug-hold-before-main");
    if(!access(hold,F_OK)){LOG("ECHOES_DEBUG_HOLD before nnMain");while(!access(hold,F_OK))usleep(10000);}
    original("nnMain");status("returned",0);return NULL;
}
static void window_created(ANativeActivity *activity,ANativeWindow *w) {
    (void)activity;pthread_mutex_lock(&window_lock);window=w;pthread_mutex_unlock(&window_lock);
    dm_vk_set_window(w);
    if(!atomic_exchange(&launched,1)) {
        pthread_attr_t attr;pthread_t thread;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,16*1024*1024);
        if(pthread_create(&thread,&attr,game_thread,NULL))die("Echoes game thread");
        pthread_attr_destroy(&attr);pthread_detach(thread);
    }
}
static void window_destroyed(ANativeActivity *activity,ANativeWindow *w) {
    (void)activity;(void)w;dm_android_input_cancel();dm_gfx_suspend(1);dm_vk_set_window(NULL);
    pthread_mutex_lock(&window_lock);window=NULL;pthread_mutex_unlock(&window_lock);
}
static void destroyed(ANativeActivity *activity) {
    dm_android_save_notice_destroy(activity);
    atomic_store(&lifecycle_destroyed,1);atomic_store(&vsync_active,0);dm_android_input_cancel();
    pthread_mutex_lock(&exit_lock);exit_pending=1;int defer=exit_sections!=0;exit_save_reason=defer?"Echoes deferred Activity exit":"Echoes Activity destroyed";unsigned sections=exit_sections;pthread_mutex_unlock(&exit_lock);
    LOG("ECHOES_ACTIVITY_DESTROYED exit_sections=%u save_request=queued",sections);
    request_exit_save();
}
JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *activity,void *saved,size_t size) {
    (void)saved;(void)size;
    if(!activity->internalDataPath || !activity->externalDataPath)die("Echoes app directories unavailable");
    input_vm=activity->vm;
    snprintf(private_root,sizeof private_root,"%s",activity->internalDataPath);
    snprintf(content_root,sizeof content_root,"%s",activity->externalDataPath);
    JNIEnv *env=activity->env;
    jclass cls=(*env)->GetObjectClass(env,activity->clazz);
    jmethodID wm_method=(*env)->GetMethodID(env,cls,"getWindowManager","()Landroid/view/WindowManager;");
    jobject wm=(*env)->CallObjectMethod(env,activity->clazz,wm_method);
    jclass wm_cls=(*env)->GetObjectClass(env,wm);
    jobject display=(*env)->CallObjectMethod(env,wm,(*env)->GetMethodID(env,wm_cls,"getDefaultDisplay","()Landroid/view/Display;"));
    jclass display_cls=(*env)->GetObjectClass(env,display);
    display_hz=(*env)->CallFloatMethod(env,display,(*env)->GetMethodID(env,display_cls,"getRefreshRate","()F"));
    jobject resources=(*env)->CallObjectMethod(env,activity->clazz,(*env)->GetMethodID(env,cls,"getResources","()Landroid/content/res/Resources;"));
    jclass resources_cls=(*env)->GetObjectClass(env,resources);
    jobject metrics=(*env)->CallObjectMethod(env,resources,(*env)->GetMethodID(env,resources_cls,"getDisplayMetrics","()Landroid/util/DisplayMetrics;"));
    jclass metrics_cls=(*env)->GetObjectClass(env,metrics);
    display_density=(*env)->GetFloatField(env,metrics,(*env)->GetFieldID(env,metrics_cls,"density","F"));
    if((*env)->ExceptionCheck(env))die("Echoes Android display query");
    (*env)->DeleteLocalRef(env,metrics_cls);(*env)->DeleteLocalRef(env,metrics);
    (*env)->DeleteLocalRef(env,resources_cls);(*env)->DeleteLocalRef(env,resources);
    (*env)->DeleteLocalRef(env,display_cls);(*env)->DeleteLocalRef(env,display);
    (*env)->DeleteLocalRef(env,wm_cls);(*env)->DeleteLocalRef(env,wm);(*env)->DeleteLocalRef(env,cls);
    char log[PATH_MAX],shaders[PATH_MAX];path(log,sizeof log,private_root,"bringup.log");
    int fd=open(log,O_CREAT|O_WRONLY|O_TRUNC,0600);if(fd<0 || dup2(fd,STDERR_FILENO)<0)die("Echoes log");close(fd);
    setvbuf(stderr,NULL,_IONBF,0);g_t0_ns=now_ns();g_flag.dbg=1;g_flag.fs=1;
    char fslog[PATH_MAX];path(fslog,sizeof fslog,private_root,"filesystem.log");setenv("DM_FSLOG",fslog,1);
    /* Explicit title-private opt-in for an optional Adreno driver bundle.
     * Ordinary runs use the system loader. Keep these markers absent on Mali. */
    unsetenv("DM_PRIVATE_VULKAN");unsetenv("TU_DEBUG");
    char driver_option[PATH_MAX];path(driver_option,sizeof driver_option,private_root,"private-vulkan");
    if(!access(driver_option,F_OK))setenv("DM_PRIVATE_VULKAN","turnip",1);
    path(driver_option,sizeof driver_option,private_root,"private-vulkan-sysmem");
    if(!access(driver_option,F_OK)){setenv("DM_PRIVATE_VULKAN","turnip",1);setenv("TU_DEBUG","sysmem",1);}
    LOG("ECHOES_VULKAN_DRIVER requested=%s sysmem=%d",getenv("DM_PRIVATE_VULKAN")?"turnip":"system",getenv("TU_DEBUG")!=NULL);
    char validation[PATH_MAX];path(validation,sizeof validation,private_root,"vulkan-validation");
    if(!access(validation,F_OK))setenv("DM_VULKAN_VALIDATION","1",1);
    char storage_bias[PATH_MAX];path(storage_bias,sizeof storage_bias,private_root,"disable-storage-bias");
    if(!access(storage_bias,F_OK))setenv("DM_DISABLE_STORAGE_BIAS","1",1);
    char draw_parameters[PATH_MAX];path(draw_parameters,sizeof draw_parameters,private_root,"portable-draw-parameters");
    if(!access(draw_parameters,F_OK))setenv("DM_PORTABLE_DRAW_PARAMETERS","1",1);
    char depth_input[PATH_MAX];path(depth_input,sizeof depth_input,private_root,"depth-input-experiment");
    if(!access(depth_input,F_OK))setenv("DM_DEPTH_INPUT_EXPERIMENT","1",1);
    char depth_ties[PATH_MAX];path(depth_ties,sizeof depth_ties,private_root,"depth-input-calibrated-ties");
    if(!access(depth_ties,F_OK))setenv("DM_DEPTH_INPUT_CALIBRATED_TIES","1",1);
    path(shaders,sizeof shaders,content_root,"shaders");dm_vk_set_shader_root(shaders);
    char captures[PATH_MAX];path(captures,sizeof captures,private_root,"shader-captures");
    if(dm_android_fs_ensure_directory(captures))die("Echoes shader capture directory");
    setenv("DM_SHADER_CAPTURE_ROOT",captures,1);
    setenv("DM_ADAPTED_SHADER_DIR",captures,1);
    setenv("DM_SHADER_IDENTITY_CODE","1",1);
    /* Diagnostics are requested by files in this directory. An ordinary run
     * does not read back images; trace-request captures only the next frame. */
    char frames[PATH_MAX];path(frames,sizeof frames,private_root,"frame-captures");
    if(dm_android_fs_ensure_directory(frames))die("Echoes frame capture directory");
    setenv("DM_FRAME_DIR",frames,1);
    choreographer=AChoreographer_getInstance();if(!choreographer)die("Echoes Choreographer unavailable");
    atomic_store(&vsync_active,1);AChoreographer_postFrameCallback64(choreographer,vsync_frame,activity);
    status("activity",0);
    activity->callbacks->onStart=activity_on_start;
    activity->callbacks->onResume=activity_on_resume;
    activity->callbacks->onPause=activity_on_pause;
    activity->callbacks->onStop=activity_on_stop;
    activity->callbacks->onWindowFocusChanged=activity_focus_changed;
    activity->callbacks->onNativeWindowCreated=window_created;
    activity->callbacks->onNativeWindowDestroyed=window_destroyed;
    activity->callbacks->onDestroy=destroyed;
    activity->callbacks->onInputQueueCreated=input_created;
    activity->callbacks->onInputQueueDestroyed=input_destroyed;
    ANativeActivity_setWindowFlags(activity,AWINDOW_FLAG_KEEP_SCREEN_ON,0);
}
