/* A real original-SDK PCM voice/config -> software DSP -> AAudio regression.
 * Uses only synthetic fixture audio and a caller-supplied pinned SDK module.
 * Never launches a game, changes saves, or claims fixture output as gameplay.
 */
#include "../../host/hle.h"
#include "../audio_renderer_android.h"
#include <math.h>
#include <unistd.h>
typedef struct {void *service,*info;} Handle;
static unsigned pcm_peak,pcm_stereo_mismatches;
void dm_android_ar_test_pcm(const int16_t *pcm,size_t frames) {
    for(size_t i=0;i<frames;i++){int value=pcm[i*2];unsigned magnitude=(unsigned)(value<0?-value:value);if(magnitude>pcm_peak)pcm_peak=magnitude;pcm_stereo_mismatches+=pcm[i*2]!=pcm[i*2+1];}
}
extern const hle_force_ent __start_dm_force[],__stop_dm_force[];
static void *sym(const char *name,int forced) {
    if(forced)for(const hle_force_ent *p=__start_dm_force;p<__stop_dm_force;p++)if(!strcmp(p->name,name))return p->fn;
    void *p=guest_sym(name);if(!p)die("Audio fixture missing symbol %s",name);return p;
}
#define ORIG(name,ret,...) ((ret (*)(__VA_ARGS__))sym(name,0))
#define HOST(name,ret,...) ((ret (*)(__VA_ARGS__))sym(name,1))
static void *aligned(size_t bytes){void *p=NULL;if(posix_memalign(&p,4096,(bytes+4095)&~4095ull))die("Audio fixture allocation");memset(p,0,(bytes+4095)&~4095ull);return p;}
int dm_android_ar_verify(const char *output) {
    uint64_t total_played=0;unsigned observed_events=0;int h0,l0,o0;void thr_counts(int *,int *,int *);thr_counts(&h0,&l0,&o0);
    for(unsigned cycle=0;cycle<3;cycle++){
        pcm_peak=pcm_stereo_mismatches=0;
        _Alignas(16) uint32_t parameter[14]={0};
        ORIG("_ZN2nn5audio32InitializeAudioRendererParameterEPNS0_22AudioRendererParameterE",void,void *)(parameter);
        parameter[2]=2;parameter[3]=0;parameter[4]=1;parameter[5]=1;parameter[6]=0;
        size_t work_bytes=HOST("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE",size_t,const void *)(parameter);
        size_t config_bytes=ORIG("_ZN2nn5audio36GetAudioRendererConfigWorkBufferSizeERKNS0_22AudioRendererParameterE",size_t,const void *)(parameter);
        void *work=aligned(work_bytes),*config_work=aligned(config_bytes),*config=aligned(4096),*pcm=aligned(32768);
        for(int i=0;i<12000;i++)((int16_t *)pcm)[i]=(int16_t)lrint(512*sin(i*2*3.141592653589793*440/48000));
        _Alignas(16) uint8_t event[256]={0};Handle handle={0};
        Result rc=HOST("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm",Result,Handle *,void *,const void *,void *,size_t)(&handle,event,parameter,work,work_bytes);
        if(rc)die("Audio fixture Open=%#x",rc);
        ORIG("_ZN2nn5audio29InitializeAudioRendererConfigEPNS0_19AudioRendererConfigERKNS0_22AudioRendererParameterEPvm",void,void *,const void *,void *,size_t)(config,parameter,config_work,config_bytes);
        void *mix=NULL,*voice=NULL,*pool=NULL,*sink=NULL;
        if(!ORIG("_ZN2nn5audio15AcquireFinalMixEPNS0_19AudioRendererConfigEPNS0_12FinalMixTypeEi",int,void *,void *,int)(config,&mix,2))die("Audio fixture final mix");
        if(!ORIG("_ZN2nn5audio17AcquireMemoryPoolEPNS0_19AudioRendererConfigEPNS0_14MemoryPoolTypeEPvm",int,void *,void *,void *,size_t)(config,&pool,pcm,32768))die("Audio fixture memory pool");
        if(!ORIG("_ZN2nn5audio23RequestAttachMemoryPoolEPNS0_14MemoryPoolTypeE",int,void *)(&pool))die("Audio fixture attach memory pool");
        if(!ORIG("_ZN2nn5audio16AcquireVoiceSlotEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEiiNS0_12SampleFormatEiPKvm",int,void *,void *,int,int,int,int,const void *,size_t)(config,&voice,48000,1,2,100,NULL,0))die("Audio fixture voice");
        ORIG("_ZN2nn5audio19SetVoiceDestinationEPNS0_19AudioRendererConfigEPNS0_9VoiceTypeEPNS0_12FinalMixTypeE",void,void *,void *,void *)(config,&voice,&mix);
        ORIG("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii",void,void *,void *,float,int,int)(&voice,&mix,1,0,0);
        ORIG("_ZN2nn5audio17SetVoiceMixVolumeEPNS0_9VoiceTypeEPNS0_12FinalMixTypeEfii",void,void *,void *,float,int,int)(&voice,&mix,1,0,1);
        const int8_t channels[2]={0,1};
        rc=ORIG("_ZN2nn5audio13AddDeviceSinkEPNS0_19AudioRendererConfigEPNS0_14DeviceSinkTypeEPNS0_12FinalMixTypeEPKaiPKc",Result,void *,void *,void *,const int8_t *,int,const char *)(config,&sink,&mix,channels,2,"MainAudioOut");
        if(rc)die("Audio fixture sink=%#x",rc);
        _Alignas(16) uint8_t wave[64]={0};uint64_t address=(uint64_t)(uintptr_t)pcm,bytes=24000;uint32_t end=12000;
        memcpy(wave,&address,8);memcpy(wave+8,&bytes,8);memcpy(wave+0x14,&end,4);wave[0x19]=1;
        if(!ORIG("_ZN2nn5audio16AppendWaveBufferEPNS0_9VoiceTypeEPKNS0_10WaveBufferE",int,void *,const void *)(&voice,wave))die("Audio fixture wave");
        ORIG("_ZN2nn5audio17SetVoicePlayStateEPNS0_9VoiceTypeENS1_9PlayStateE",void,void *,int)(&voice,0);
        Result (*update)(Handle,const void *)=HOST("_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE",Result,Handle,const void *);
        if((rc=update(handle,config)))die("Audio fixture initial update=%#x",rc);
        HOST("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE",void,Handle)(handle);
        uint64_t deadline=now_ns()+5000000000ull,played=0;
        while(played<4800){
            if(now_ns()>deadline)die("Audio fixture playback timeout samples=%llu",played);
            if(!(ORIG("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE",int,void *)(event)&1)){usleep(1000);continue;}
            observed_events++;if((rc=update(handle,config)))die("Audio fixture update=%#x",rc);
            uint64_t next=ORIG("_ZN2nn5audio25GetVoicePlayedSampleCountEPKNS0_9VoiceTypeE",uint64_t,const void *)(&voice);
            if(next<played)die("Audio fixture played sample regression previous=%llu current=%llu",played,next);played=next;
        }
        HOST("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE",void,Handle)(handle);
        dm_android_ar_metrics stopped,after;dm_android_ar_snapshot(&stopped);usleep(30000);dm_android_ar_snapshot(&after);
        if(after.rendered_blocks!=stopped.rendered_blocks||after.started_renderers||!after.nonzero_samples||after.last_error)die("Audio fixture stop/output metrics");
        if(pcm_peak<500||pcm_peak>514||pcm_stereo_mismatches||!after.device_read_frames)die("Audio fixture PCM amplitude/channels/device clock peak=%u mismatch=%u read=%llu",pcm_peak,pcm_stereo_mismatches,after.device_read_frames);
        HOST("_ZN2nn5audio18StartAudioRendererENS0_19AudioRendererHandleE",void,Handle)(handle);
        deadline=now_ns()+2000000000ull;
        do{usleep(1000);dm_android_ar_snapshot(&after);if(now_ns()>deadline)die("Audio fixture restart timeout");}while(after.rendered_blocks<stopped.rendered_blocks+3);
        HOST("_ZN2nn5audio17StopAudioRendererENS0_19AudioRendererHandleE",void,Handle)(handle);
        HOST("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE",void,Handle)(handle);
        ORIG("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE",void,void *)(event);
        dm_android_ar_snapshot(&after);if(after.open_renderers)die("Audio fixture renderer leaked");
        int h1,l1,o1;thr_counts(&h1,&l1,&o1);if(h0!=h1||l0!=l1||o0!=o1)die("Audio fixture thread/event handle leak");
        total_played+=played;free(pcm);free(config);free(config_work);free(work);
        LOG("AUDIO_RENDERER_FIXTURE cycle=%u played_samples=%llu events=%u nonzero=%llu",cycle,played,observed_events,after.nonzero_samples);
    }
    dm_android_ar_metrics m;dm_android_ar_snapshot(&m);FILE *f=fopen(output,"w");if(!f)die("Audio fixture result");
    fprintf(f,"{\"passed\":true,\"fixture_audio\":true,\"game_frame\":false,\"cycles\":3,\"played_samples\":%llu,\"observed_events\":%u,\"written_frames\":%llu,\"device_read_frames\":%llu,\"nonzero_samples\":%llu,\"pcm_peak\":%u,\"stereo_channels_equal\":true,\"restart_checked\":true,\"thread_event_handles_leaked\":false,\"open_renderers\":%d}\n",total_played,observed_events,m.written_frames,m.device_read_frames,m.nonzero_samples,pcm_peak,m.open_renderers);
    if(fclose(f))die("Audio fixture result close");return 0;
}
