/* Shared production controller and checked 94012 adapter, simulated monotonic
 * clock, synthetic renderer, original wrapper prologue and native callback.
 * No original game, Activity or player save is executed. */
#include "../../host/hle_oe.c"
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include "../render_scale.h"

static unsigned checks,rebuilds,writes;
static uint64_t fake_now=100*S3_SEC,frames=100;
static int in_game=1,thermal=0,ruler,broken_rebuild;
static uint8_t *renderer;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"scale3d CHECK failed line=%d\n",__LINE__);exit(1);}}while(0)
module_t g_mod[MAX_MOD];int g_nmod;
uint64_t now_ns(void){return fake_now;}
uint64_t dm_nvn_frame_count(void){return frames;}
int dm_nvn_ruler_truth(void){return ruler;}
int dm_ingame_now(void){return in_game;}
int dm_android_thermal_status_now(void){return thermal;}
void dm_guest_write_prepare(void *p,size_t n){CHECK(p==renderer+0x66a4&&n==4);writes++;}
void dm_ui_gen_bump(int why){(void)why;}
void hle_bind_system_event(void *p,int ready){(void)p;(void)ready;}
void hle_signal_system_event(void *p){(void)p;}
void dm_android_clear_system_event(void *p){(void)p;}
void dm_android_platform_setting(const char *key,int value){(void)key;(void)value;}
uint64_t dm_android_platform_language_code(void){return 0;}
void dm_android_platform_device_uuid(uint8_t bytes[16]){memset(bytes,0,16);}

static void rebuild(uint32_t width,uint32_t height) {
    CHECK(width==1280&&height==720);rebuilds++;
    float scale;memcpy(&scale,renderer+0x66a4,4);
    uint32_t sizes[]={lroundf(width*scale),lroundf(height*scale),lroundf(width*scale),lroundf(height*scale),width,height};
    if(broken_rebuild)sizes[broken_rebuild==1?0:4]++;
    memcpy(renderer+0xee78,sizes,sizeof sizes);
    /* Original rebuild can enter host APIs: neither state lock nor another
     * rebuild may be held across the game call. */
    dm_android_scale3d_status status;dm_android_scale3d_status_get(&status);
    android_scale3d_tick(g_mod[0].base+0xe10d58);
}
static void wrapper_stub(void) {
    /* Original 8582f0 prologue branches here when its singleton is null.
     * Call the recorder, then restore its real saved register/stack frame.
     * All 67 checked original instructions remain unmodified. */
    uint32_t code[]={0x58000110,0xd63f0200,0xa9495ff8,0xa94a57f6,0xa94b4ff4,0xa94c7bfd,0x910343ff,0xd65f03c0};
    uint8_t *p=g_mod[0].base+0x8584b4;void *fn=rebuild;memcpy(p,code,sizeof code);memcpy(p+sizeof code,&fn,8);
    __builtin___clear_cache((char *)p,(char *)p+sizeof code+8);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);uintptr_t begin=(uintptr_t)(g_mod[0].base+0x8582f0)&~(page-1);
    CHECK(!mprotect((void *)begin,page,PROT_READ|PROT_EXEC));
}
static void tick(void){android_scale3d_tick(g_mod[0].base+0xe10d58);}
static void present(unsigned n,uint64_t interval,uint64_t target) {
    for(unsigned i=0;i<n;i++){fake_now+=interval;frames++;dm_android_scale3d_present(fake_now,target);}
}
static void *wrong_thread(void *unused){(void)unused;tick();return NULL;}
static void setup(void) {
    CHECK(!unsetenv("DM_3D_SCALE"));CHECK(!unsetenv("DM_3D_SCALE_FLOOR"));CHECK(!unsetenv("DM_3D_SCALE_DYN"));CHECK(!unsetenv("DM_THERMAL_NOFALLBACK"));
    module_t *m=&g_mod[0];g_nmod=1;strcpy(m->name,"main");strcpy(m->build_id,"db0507f36b84200ac05f75d76ed5026f");
    m->text_end=0x1b786a0;m->ro_off=0x1b79000;m->data_off=0x2056000;m->size=0x3220000;
    m->base=mmap(NULL,m->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(m->base!=MAP_FAILED);
    for(size_t i=0;i<sizeof dm_s3_anchors/sizeof *dm_s3_anchors;i++)memcpy(m->base+dm_s3_anchors[i].address,dm_s3_anchors[i].words,dm_s3_anchors[i].count*4);
    CHECK(!posix_memalign((void **)&renderer,16,0x20000));memset(renderer,0,0x20000);
    uintptr_t ptr=(uintptr_t)renderer;memcpy(m->base+0x31e0848,&ptr,8);
    float initial=1;memcpy(renderer+0x66a4,&initial,4);
    uint32_t sizes[]={1280,720,1280,720,1280,720};memcpy(renderer+0xee78,sizes,sizeof sizes);
}
static void expect_bad_rebuild(int kind) {
    pid_t pid=fork();CHECK(pid>=0);if(!pid){broken_rebuild=kind;dm_3d_quality_mode_set(1);tick();_exit(99);}
    int result;CHECK(waitpid(pid,&result,0)==pid);CHECK(WIFSIGNALED(result)&&WTERMSIG(result)==SIGABRT);
}
static void config_case(const char *key,const char *value,float base,float floor,int max_level,int dynamic,int invalid) {
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid) {
        CHECK(!setenv(key,value,1));
        if(invalid)CHECK(freopen("/dev/null","w",stderr));
        android_s3_configure();
        CHECK(android_s3.configured==base&&android_s3.floor==floor&&android_s3.max_level==max_level&&android_s3_dynamic==dynamic);
        _exit(0);
    }
    int result;CHECK(waitpid(pid,&result,0)==pid);
    CHECK(invalid?(WIFSIGNALED(result)&&WTERMSIG(result)==SIGABRT):(WIFEXITED(result)&&!WEXITSTATUS(result)));
}
static void controller_frames(scale3d_ctl *c,uint64_t *now,unsigned count,double interval) {
    for(unsigned i=0;i<count;i++) {
        *now+=(uint64_t)lround(interval>0?interval*1e6:c->period_ms*1e6);
        scale3d_ctl_step(c,interval,*now);
    }
}
static void controller_boundaries(void) {
    /* A real recovery followed by pressure within 15 seconds must delay the
     * next recovery to 90 clean seconds, even though 30 were enough before. */
    scale3d_ctl c={.level=1,.max_level=3,.period_ms=20};uint64_t now=S3_SEC;
    controller_frames(&c,&now,1500,20);CHECK(c.level==0&&c.last_up==now);
    now=c.hold_until;controller_frames(&c,&now,250,40);
    CHECK(c.level==1&&c.need_good==45);
    now=c.hold_until;controller_frames(&c,&now,4450,20);CHECK(c.level==1);
    controller_frames(&c,&now,50,20);CHECK(c.level==0&&!c.need_good);
    /* At 2% drops the fast 5% rule must stay clear, and the 60 second 1.5%
     * rule must still lower. -1 is the shared controller's dropped sample. */
    c=(scale3d_ctl){.max_level=3,.period_ms=10};now=S3_SEC;
    for(unsigned window=0;window<30;window++) {
        controller_frames(&c,&now,196,10);controller_frames(&c,&now,4,-1);
        CHECK(c.level==(window==29?1:0));
    }
}
int main(void) {
    alarm(20);setup();module_t *m=&g_mod[0];CHECK(android_s3_profile(m));
    /* Status polling before native configuration must not freeze environment
     * settings. Configuration children run before the parent's pthread_once. */
    CHECK(!setenv("DM_3D_SCALE","0.8",1));dm_android_scale3d_status initial;
    dm_android_scale3d_status_get(&initial);CHECK(initial.configured==.75f);CHECK(!unsetenv("DM_3D_SCALE"));
    config_case("DM_3D_SCALE","0.5",.5f,.6f,3,1,0);
    config_case("DM_3D_SCALE","1.0",1.f,.6f,3,1,0);
    config_case("DM_3D_SCALE_FLOOR","0.5",.75f,.5f,5,1,0);
    config_case("DM_3D_SCALE_FLOOR","1.0",.75f,1.f,0,1,0);
    config_case("DM_3D_SCALE_DYN","0",.75f,.6f,3,0,0);
    config_case("DM_THERMAL_NOFALLBACK","1",.75f,.6f,3,0,0);
    config_case("DM_3D_SCALE","nan",0,0,0,0,1);
    config_case("DM_3D_SCALE_FLOOR","0.49",0,0,0,0,1);
    config_case("DM_3D_SCALE","0.75x",0,0,0,0,1);
    controller_boundaries();
    /* Each independent instruction range, build ID and layout gate rejects. */
    m->build_id[0]=0;CHECK(!android_s3_profile(m));tick();CHECK(!writes);m->build_id[0]='d';
    m->build_id[0]='a';CHECK(!android_s3_profile(m));tick();CHECK(!writes);m->build_id[0]='d';
    m->text_end--;CHECK(!android_s3_profile(m));tick();CHECK(!writes);m->text_end++;
    m->ro_off--;tick();CHECK(!writes);m->ro_off++;
    m->data_off--;tick();CHECK(!writes);m->data_off++;
    m->size=0x3213fff;tick();CHECK(!writes);m->size=0x3220000;
    for(size_t i=0;i<sizeof dm_s3_anchors/sizeof *dm_s3_anchors;i++) {
        uint32_t *word=(uint32_t *)(m->base+dm_s3_anchors[i].address);*word^=1;
        CHECK(!android_s3_profile(m));tick();CHECK(!writes);*word^=1;
    }
    android_scale3d_tick(m->base+0xe10d5c);CHECK(!writes);
    frames=29;tick();CHECK(!writes);frames=100;
    uintptr_t valid=(uintptr_t)renderer,bad=1;memcpy(m->base+0x31e0848,&bad,8);tick();CHECK(!writes);
    memcpy(m->base+0x31e0848,&valid,8);float bad_scale=NAN;memcpy(renderer+0x66a4,&bad_scale,4);tick();CHECK(!writes);
    bad_scale=1;memcpy(renderer+0x66a4,&bad_scale,4);wrapper_stub();tick();
    CHECK(rebuilds==1&&writes==1&&dm_3d_scale_cur()==.75f);CHECK(android_s3_profile(m));
    CHECK(*(uint32_t *)(renderer+0xee78)==960&&*(uint32_t *)(renderer+0xee7c)==540);
    CHECK(*(uint32_t *)(renderer+0xee88)==1280&&*(uint32_t *)(renderer+0xee8c)==720);
    tick();CHECK(rebuilds==1);ruler=1;pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,wrong_thread,NULL));CHECK(!pthread_join(thread,NULL));
    CHECK(rebuilds==1&&writes==1&&dm_3d_scale_cur()==.75f);ruler=0;
    const uint64_t p60=1000000000ull/60,p30=1000000000ull/30;
    dm_android_scale3d_present(fake_now,p60);present(600,p30,p60);CHECK(android_s3.level==0); /* 20 s load hold */
    present(310,p30,p60);CHECK(android_s3.level==1);tick();CHECK(dm_3d_scale_cur()==.70f);
    dm_3d_scale_resumed();present(60,p30,p60);CHECK(android_s3.level==1); /* Recovery samples excluded. */
    present(2200,p60,p60);CHECK(android_s3.level==0);tick();CHECK(dm_3d_scale_cur()==.75f);
    thermal=6;present(120,p60,p60);CHECK(android_s3.thermal_status==6&&android_s3.level==0);
    present(1500,p30,p30);CHECK(android_s3.level==0&&android_s3.target_period_ns==p30); /* eco toggle */
    present(900,50000000,50000000);CHECK(android_s3.level==0); /* slower 20 FPS guest */
    present(120,100000000,0);CHECK(android_s3.level==0); /* explicit uncapped diagnostic */
    present(1600,p30,p60);CHECK(android_s3.level==3);tick();CHECK(dm_3d_scale_cur()==.60f);
    CHECK(*(uint32_t *)(renderer+0xee78)==768&&*(uint32_t *)(renderer+0xee7c)==432);
    CHECK(*(uint32_t *)(renderer+0xee88)==1280&&*(uint32_t *)(renderer+0xee8c)==720);
    expect_bad_rebuild(1);expect_bad_rebuild(2); /* wrong 3D size or changed UI output aborts */
    dm_3d_quality_mode_set(1);tick();CHECK(dm_3d_scale_cur()==.75f&&dm_3d_quality_mode_get()==1);
    present(2000,p30,p60);CHECK(android_s3.level==0);tick();CHECK(dm_3d_scale_cur()==.75f);
    dm_3d_quality_mode_set(0);present(120,p30,p60);CHECK(android_s3.level==0); /* switch hold */
    in_game=0;present(1000,p30,p60);CHECK(android_s3.level==0);
    in_game=1;present(500,p30,p60);CHECK(android_s3.level==0); /* fresh run warmup */
    dm_focus_set(2);dm_focus_set(1);CHECK(atomic_load(&g_scale3d_resumed));
    dm_android_scale3d_status status;dm_android_scale3d_status_get(&status);
    CHECK(status.supported&&status.current==.75f&&status.changes==rebuilds&&status.max_level==3);
    CHECK(status.output_width==1280&&status.output_height==720&&status.last_caller==0xe10d58);
    android_scale3d_tick(m->base+0xe10db0);dm_android_scale3d_status_get(&status);CHECK(status.last_caller==0xe10db0);
    m->build_id[0]='a';tick();dm_android_scale3d_status_get(&status);CHECK(!status.supported&&status.changes==rebuilds);
    /* main's two fixed choices override dynamic pressure and stale levels. */
    m->build_id[0]='d';CHECK(dm_android_render_scale_requested()==1.f);
    CHECK(dm_android_render_scale_request(NAN)==EINVAL);
    CHECK(dm_android_render_scale_request(INFINITY)==EINVAL);
    CHECK(dm_android_render_scale_request(.69f)==EINVAL);
    CHECK(dm_android_render_scale_request(1.01f)==EINVAL);
    CHECK(!dm_android_render_scale_request(1.f));tick();
    CHECK(dm_3d_scale_cur()==1.f&&dm_android_render_scale_requested()==1.f);
    present(4000,p30,p60);tick();CHECK(android_s3.level==0&&dm_3d_scale_cur()==1.f);
    CHECK(*(uint32_t *)(renderer+0xee78)==1280&&*(uint32_t *)(renderer+0xee7c)==720);
    CHECK(!dm_android_render_scale_request(.75f));tick();
    present(4000,p30,p60);tick();CHECK(android_s3.level==0&&dm_3d_scale_cur()==.75f);
    CHECK(dm_android_render_scale_requested()==.75f);
    CHECK(*(uint32_t *)(renderer+0xee78)==960&&*(uint32_t *)(renderer+0xee7c)==540);
    CHECK(*(uint32_t *)(renderer+0xee88)==1280&&*(uint32_t *)(renderer+0xee8c)==720);
    dm_android_scale3d_status_get(&status);CHECK(status.configured==.75f);
    CHECK(!dm_android_render_scale_request(1.f));tick();dm_android_scale3d_status_get(&status);
    CHECK(status.current==1.f&&status.configured==1.f&&status.changes==rebuilds);
    printf("ANDROID_D2R_SCALE3D_OK checks=%u rebuilds=%u anchors=7 build94012=1 real_controller=1 loading_hold=1 resume_hold=1 eco_period=1 slow_guest_period=1 quality_switch=1 thermal_observation=1 floor=0.60 ui_output_unchanged=1 wrong_thread_rejected=1 rebuild_postconditions=1 anti_flap=1 slow_drop_threshold=1 config_boundaries=1 synthetic_renderer=1 actual_game_not_claimed=1 fixed_request_overrides_dynamic=1\n",checks,rebuilds);
    return 0;
}
