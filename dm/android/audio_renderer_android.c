/* Echoes 1.1.0 / SDK 20.5 Android AudioRenderer bridge.
 * Keep the original SDK's config serializer, service implementation, decoders,
 * effects and command processor. Select its supported manual CPU backend and
 * drive it with real AAudio output backpressure. No Horizon IPC is fabricated.
 * ABI facts are pinned to this title's SDK, not shared with the Apple host. */
#include "../host/hle.h"
#include "audio_renderer_android.h"
#include <aaudio/AAudio.h>
#include <stdatomic.h>
#include <unistd.h>
#include <math.h>

typedef struct {void *service,*info;} ArHandle;
typedef struct Ar {
    ArHandle handle;
    void *event,*stack,*thread;
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int started,closing,busy,suspended,nonzero_logged,frames;
    AAudioStream *stream;
    float mix[240*2];
    int16_t pcm[240*2];
    uint64_t blocks;
} Ar;
static pthread_mutex_t ar_registry_lock=PTHREAD_MUTEX_INITIALIZER;
static Ar *ar_registry[4];
static _Thread_local Ar *ar_current;
static _Atomic uint64_t ar_updates,ar_blocks,ar_frames,ar_nonzero,ar_sinks,ar_events;
static uint64_t ar_closed_read_frames; /* protected by ar_registry_lock */
static _Atomic int ar_error;
static _Atomic float ar_device_volume=1.0f;
extern void hle_bind_system_event(void *,int);
extern void hle_signal_system_event(void *);
extern void dm_android_unbind_system_event(void *);

static void *ar_symbol(const char *name) {
    void *p=guest_sym(name);if(!p)die("AudioRenderer SDK symbol missing: %s",name);return p;
}
static Ar *ar_find(ArHandle handle) {
    pthread_mutex_lock(&ar_registry_lock);Ar *r=NULL;
    for(unsigned i=0;i<4;i++)if(ar_registry[i]&&ar_registry[i]->handle.service==handle.service){r=ar_registry[i];break;}
    pthread_mutex_unlock(&ar_registry_lock);
    if(!r || r->handle.info!=handle.info)die("AudioRenderer invalid host handle");return r;
}
static void ar_cpu_parameter(uint8_t dst[56],const void *src) {
    memcpy(dst,src,56);uint32_t *p=(uint32_t *)dst;
    if(p[0]!=48000 || p[1]!=240 || p[11]!=0 || p[12]>1)die("AudioRenderer unsupported rate/frame/mode: %u/%u/%u/%u",p[0],p[1],p[11],p[12]);
    p[11]=1; /* Manual execution, driven by the Android device rather than Horizon. */
    p[12]=1; /* Original SDK CPU DSP command processor. */
}
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "Original SDK computes the actual work size for the Android CPU renderer", size_t, ar_work_size,const void *parameter) {
    _Alignas(8) uint8_t local[56];ar_cpu_parameter(local,parameter);
    size_t size=((size_t (*)(const void *))ar_symbol("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE"))(local);
    const uint32_t *p=parameter;
    LOG("AUDIO_RENDERER_WORK bytes=%zu rate=%u samples=%u mix_buffers=%u submixes=%u voices=%u sinks=%u effects=%u execution=%u device=%u revision=%#x",size,p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[11],p[12],p[13]);
    return size;
}
HLE_FORCE("_ZN2nn6applet23GetAppletResourceUserIdEv", "Android process-owned audio resource identity; no Horizon applet session", uint64_t, ar_resource_user,void) {
    return (uint64_t)(uint32_t)getpid()<<32 | (uint32_t)getuid();
}
static void ar_audio_check(int result,const char *stage) {
    if(result<0){atomic_store(&ar_error,result);die("AudioRenderer AAudio %s error=%d %s",stage,result,AAudio_convertResultToText(result));}
}
static void ar_stream_open(Ar *r) {
    AAudioStreamBuilder *b=NULL;ar_audio_check(AAudio_createStreamBuilder(&b),"builder");
    AAudioStreamBuilder_setDirection(b,AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setSampleRate(b,48000);AAudioStreamBuilder_setChannelCount(b,2);
    AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setUsage(b,AAUDIO_USAGE_GAME);AAudioStreamBuilder_setContentType(b,AAUDIO_CONTENT_TYPE_MUSIC);
    AAudioStreamBuilder_setPerformanceMode(b,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    int result=AAudioStreamBuilder_openStream(b,&r->stream);AAudioStreamBuilder_delete(b);ar_audio_check(result,"open");
    if(AAudioStream_getSampleRate(r->stream)!=48000 || AAudioStream_getChannelCount(r->stream)!=2 || AAudioStream_getFormat(r->stream)!=AAUDIO_FORMAT_PCM_I16)
        die("AudioRenderer AAudio negotiated unsupported format");
    int burst=AAudioStream_getFramesPerBurst(r->stream);if(burst>0&&burst<4096)AAudioStream_setBufferSizeInFrames(r->stream,burst*2);
    LOG("AUDIO_RENDERER_AAUDIO rate=48000 channels=2 burst=%d device=%d",burst,AAudioStream_getDeviceId(r->stream));
}
HLE_FORCE("_ZN2nn5audio19ListAudioDeviceNameEPNS0_15AudioDeviceNameEi", "Expose the actual Android-backed primary AudioRenderer output", int, ar_list_devices,char *names,int count) {
    if(count<=0)return 0;if(!names)die("AudioRenderer null device list");
    memset(names,0,256);strcpy(names,"MainAudioOut");return 1;
}
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "Apply primary device gain to rendered Android PCM", void, ar_set_volume,const char *name,float volume) {
    if(!name || strcmp(name,"MainAudioOut") || !isfinite(volume) || volume<0 || volume>1)die("AudioRenderer invalid device/volume");
    atomic_store(&ar_device_volume,volume);
}
/* SDK 20.5 dispatch's DeviceSink case is inlined away in CPU mode. Intercept
 * that one command at the original dispatch boundary; delegate every other
 * command to the unmodified original SDK dispatcher. */
HLE_FORCE("_ZN2nn5audio3dsp20CommandListProcessor15DispatchCommandEPKNS0_13CommandHeaderE", "Route the original CPU DSP DeviceSink command to Android PCM output", void, ar_dispatch,void *processor,const uint8_t *command) {
    if(command[5]!=0x14){((void (*)(void *,const void *))ar_symbol("_ZN2nn5audio3dsp20CommandListProcessor15DispatchCommandEPKNS0_13CommandHeaderE"))(processor,command);return;}
    Ar *r=ar_current;if(!r)die("AudioRenderer device sink outside renderer thread");
    uint16_t size;memcpy(&size,command+6,2);if(size!=0x148)die("AudioRenderer device sink command size=%u",size);
    int frames,buffers,channels;const int32_t *mix;
    memcpy(&frames,(uint8_t *)processor+0x20,4);memcpy(&buffers,(uint8_t *)processor+0x28,4);memcpy(&mix,(uint8_t *)processor+0x30,8);
    memcpy(&channels,command+0x130,4);
    if(frames!=r->frames || (channels!=1&&channels!=2&&channels!=6) || !mix || buffers<1)die("AudioRenderer device sink shape frames=%d channels=%d buffers=%d",frames,channels,buffers);
    int16_t ids[6];memcpy(ids,command+0x134,12);
    for(int ch=0;ch<channels;ch++)if(ids[ch]<0 || ids[ch]>=buffers)die("AudioRenderer mix buffer index=%d/%d",ids[ch],buffers);
    for(int i=0;i<frames;i++) {
        float left=mix[(size_t)ids[0]*frames+i],right=channels==1?left:(float)mix[(size_t)ids[1]*frames+i];
        if(channels==6){float center=mix[(size_t)ids[2]*frames+i]*0.70710678f;
            left+=center+mix[(size_t)ids[4]*frames+i]*0.70710678f;
            right+=center+mix[(size_t)ids[5]*frames+i]*0.70710678f;}
        r->mix[i*2]+=left;r->mix[i*2+1]+=right;
    }
    atomic_fetch_add(&ar_sinks,1);
}
static uint64_t ar_prepare_locked(Ar *r) {
    memset(r->mix,0,sizeof r->mix);ar_current=r;
    Result result=((Result (*)(ArHandle))ar_symbol("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE"))(r->handle);
    ar_current=NULL;if(result)die("AudioRenderer original CPU rendering failed=%#x",result);
    uint64_t nonzero=0;
    float volume=atomic_load(&ar_device_volume);
    for(int i=0;i<r->frames*2;i++){
        float value=r->suspended?0:r->mix[i]*volume;if(!isfinite(value))die("AudioRenderer non-finite output");
        if(value>32767)value=32767;if(value<-32768)value=-32768;
        r->pcm[i]=(int16_t)lrintf(value);nonzero+=r->pcm[i]!=0;
    }
    return nonzero;
}
static void ar_write_block(Ar *r,uint64_t nonzero) {
    int offset=0;
    while(offset<r->frames){int written=AAudioStream_write(r->stream,r->pcm+offset*2,r->frames-offset,1000000000ll);
        ar_audio_check(written,"write");if(!written)die("AudioRenderer AAudio write timed out");offset+=written;}
#ifdef DM_AUDIO_RENDERER_TEST
    void dm_android_ar_test_pcm(const int16_t *,size_t);
    dm_android_ar_test_pcm(r->pcm,r->frames);
#endif
    atomic_fetch_add(&ar_blocks,1);atomic_fetch_add(&ar_frames,r->frames);atomic_fetch_add(&ar_nonzero,nonzero);
    if(nonzero&&!r->nonzero_logged){r->nonzero_logged=1;LOG("AUDIO_RENDERER_FIRST_PCM nonzero_samples=%llu block=%llu",nonzero,r->blocks+1);}
    /* Our event is intentionally not installed in the SDK manual handle: the
     * game is released only after this real block has entered the device queue. */
    if(r->event){hle_signal_system_event(r->event);atomic_fetch_add(&ar_events,1);}
    if(++r->blocks<=3 || r->blocks%1000==0)LOG("AUDIO_RENDERER_BLOCK blocks=%llu nonzero=%llu sinks=%llu",r->blocks,nonzero,(unsigned long long)atomic_load(&ar_sinks));
}
static void ar_worker(void *context) {
    Ar *r=context;pthread_mutex_lock(&r->lock);
    for(;;){while(!r->started&&!r->closing)pthread_cond_wait(&r->changed,&r->lock);
        if(r->closing)break;r->busy=1;uint64_t nonzero=ar_prepare_locked(r);
        /* Device backpressure can block for a burst. The original renderer
         * and PCM snapshot are already stable, so config updates must be free
         * to run while Android consumes this block. Stop waits for busy=0
         * before pausing the stream. */
        pthread_mutex_unlock(&r->lock);ar_write_block(r,nonzero);pthread_mutex_lock(&r->lock);
        r->busy=0;pthread_cond_broadcast(&r->changed);
    }
    pthread_mutex_unlock(&r->lock);
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "Original in-process CPU AudioRenderer, actual AAudio sink and SDK completion event", Result, ar_open,ArHandle *handle,void *event,const void *parameter,void *work,size_t size) {
    _Alignas(8) uint8_t local[56];ar_cpu_parameter(local,parameter);
    Ar *r=calloc(1,sizeof *r);if(!r)return RES_FAIL;r->frames=240;r->event=event;
    pthread_mutex_init(&r->lock,NULL);pthread_cond_init(&r->changed,NULL);
    Result result=((Result (*)(ArHandle *,const void *,void *,size_t))ar_symbol("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm"))(handle,local,work,size);
    if(result){pthread_mutex_destroy(&r->lock);pthread_cond_destroy(&r->changed);free(r);return result;}
    r->handle=*handle;if(event)hle_bind_system_event(event,1);ar_stream_open(r);
    if(posix_memalign(&r->stack,4096,256*1024) || !(r->thread=calloc(1,4096)))die("AudioRenderer worker allocation");
    Result (*create)(void *,void (*)(void *),void *,void *,size_t,int)=ar_symbol("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi");
    result=create(r->thread,ar_worker,r,r->stack,256*1024,28);if(result)die("AudioRenderer SDK worker create=%#x",result);
    pthread_mutex_lock(&ar_registry_lock);unsigned slot=0;while(slot<4&&ar_registry[slot])slot++;if(slot==4)die("AudioRenderer host session limit");ar_registry[slot]=r;pthread_mutex_unlock(&ar_registry_lock);
    ((void (*)(void *))ar_symbol("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"))(r->thread);
    LOG("AUDIO_RENDERER_OPEN service=%p work=%zu original_execution=%u original_device=%u",handle->service,size,((const uint32_t *)parameter)[11],((const uint32_t *)parameter)[12]);return RES_OK;
}
HLE_FORCE("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE", "Original SDK serialization and server validation, serialized with CPU rendering", Result, ar_update,ArHandle handle,const void *config) {
    Ar *r=ar_find(handle);pthread_mutex_lock(&r->lock);
    Result result=((Result (*)(ArHandle,const void *))ar_symbol("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE"))(handle,config);
    pthread_mutex_unlock(&r->lock);uint64_t updates=atomic_fetch_add(&ar_updates,1)+1;
    if(updates<=3||updates%200==0)LOG("AUDIO_RENDERER_UPDATE requests=%llu result=%#x",updates,result);return result;
}
HLE_FORCE("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE", "Start the original CPU renderer and actual Android stream", void, ar_start,ArHandle handle) {
    Ar *r=ar_find(handle);pthread_mutex_lock(&r->lock);
    if(!r->started){((void (*)(ArHandle))ar_symbol("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE"))(handle);
        ar_audio_check(AAudioStream_requestStart(r->stream),"start");r->started=1;pthread_cond_broadcast(&r->changed);}
    pthread_mutex_unlock(&r->lock);
}
HLE_FORCE("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE", "Stop Android device rendering before original SDK state transition", void, ar_stop,ArHandle handle) {
    Ar *r=ar_find(handle);pthread_mutex_lock(&r->lock);
    r->started=0;while(r->busy)pthread_cond_wait(&r->changed,&r->lock);
    ar_audio_check(AAudioStream_requestPause(r->stream),"pause");
    ((void (*)(ArHandle))ar_symbol("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE"))(handle);pthread_mutex_unlock(&r->lock);
}
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "Join the SDK renderer worker and close real output before releasing original service", void, ar_close,ArHandle handle) {
    Ar *r=ar_find(handle);ar_stop(handle);pthread_mutex_lock(&r->lock);r->closing=1;pthread_cond_broadcast(&r->changed);pthread_mutex_unlock(&r->lock);
    ((void (*)(void *))ar_symbol("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"))(r->thread);
    ((void (*)(void *))ar_symbol("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"))(r->thread);
    if(r->event)dm_android_unbind_system_event(r->event);
    pthread_mutex_lock(&ar_registry_lock);
    int64_t read=AAudioStream_getFramesRead(r->stream);if(read>0)ar_closed_read_frames+=(uint64_t)read;
    for(unsigned i=0;i<4;i++)if(ar_registry[i]==r)ar_registry[i]=NULL;
    pthread_mutex_unlock(&ar_registry_lock);
    ar_audio_check(AAudioStream_close(r->stream),"close");
    ((void (*)(ArHandle))ar_symbol("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE"))(handle);
    free(r->thread);free(r->stack);pthread_mutex_destroy(&r->lock);pthread_cond_destroy(&r->changed);free(r);
}
void dm_android_ar_suspend(int suspended) {
    pthread_mutex_lock(&ar_registry_lock);
    for(unsigned i=0;i<4;i++)if(ar_registry[i]){
        pthread_mutex_lock(&ar_registry[i]->lock);ar_registry[i]->suspended=suspended;pthread_mutex_unlock(&ar_registry[i]->lock);
    }
    pthread_mutex_unlock(&ar_registry_lock);
}
void dm_android_ar_snapshot(dm_android_ar_metrics *m) {
    memset(m,0,sizeof *m);m->updates=atomic_load(&ar_updates);m->rendered_blocks=atomic_load(&ar_blocks);m->written_frames=atomic_load(&ar_frames);
    m->nonzero_samples=atomic_load(&ar_nonzero);m->sink_commands=atomic_load(&ar_sinks);m->completion_events=atomic_load(&ar_events);m->last_error=atomic_load(&ar_error);
    pthread_mutex_lock(&ar_registry_lock);m->device_read_frames=ar_closed_read_frames;
    for(unsigned i=0;i<4;i++)if(ar_registry[i]){Ar *r=ar_registry[i];pthread_mutex_lock(&r->lock);m->open_renderers++;m->started_renderers+=r->started;int64_t read=AAudioStream_getFramesRead(r->stream);if(read>0)m->device_read_frames+=(uint64_t)read;pthread_mutex_unlock(&r->lock);}pthread_mutex_unlock(&ar_registry_lock);
}
