/* Independent 91738 profile and original wrapper prologue. Reuse the host
 * stubs, simulated clock and rebuild recorder from the 94012 fixture. */
#define main scale3d_94012_fixture_main
#include "t_d2r_scale3d_device.c"
#undef main

static void tick91738(void) {android_scale3d_tick(g_mod[0].base+0xdf7bb8);}
static void *wrong_thread91738(void *unused) {(void)unused;tick91738();return NULL;}
static void setup91738(void) {
    CHECK(!unsetenv("DM_3D_SCALE"));CHECK(!unsetenv("DM_3D_SCALE_FLOOR"));
    CHECK(!unsetenv("DM_3D_SCALE_DYN"));CHECK(!unsetenv("DM_THERMAL_NOFALLBACK"));
    module_t *m=&g_mod[0];g_nmod=1;strcpy(m->name,"main");strcpy(m->build_id,"2c7f979610e9911979982dc07bfc5702");
    m->text_end=0x1b4f5d0;m->ro_off=0x1b50000;m->data_off=0x2028000;m->size=0x31c0000;
    m->base=mmap(NULL,m->size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(m->base!=MAP_FAILED);
    for(size_t i=0;i<sizeof dm_s3_91738_anchors/sizeof *dm_s3_91738_anchors;i++)
        memcpy(m->base+dm_s3_91738_anchors[i].address,dm_s3_91738_anchors[i].words,dm_s3_91738_anchors[i].count*4);
    CHECK(!posix_memalign((void **)&renderer,16,0x20000));memset(renderer,0,0x20000);
    uintptr_t ptr=(uintptr_t)renderer;memcpy(m->base+0x31b2528,&ptr,8);
    float initial=1;memcpy(renderer+0x66a4,&initial,4);
    uint32_t sizes[]={1280,720,1280,720,1280,720};memcpy(renderer+0xee78,sizes,sizeof sizes);
}
static void stub91738(void) {
    /* Same verified wrapper frame shape as 94012, different entry/branch.
     * The recorder branch starts immediately after the guarded last range. */
    uint32_t code[]={0x58000110,0xd63f0200,0xa9495ff8,0xa94a57f6,0xa94b4ff4,0xa94c7bfd,0x910343ff,0xd65f03c0};
    uint8_t *p=g_mod[0].base+0x841704;void *fn=rebuild;memcpy(p,code,sizeof code);memcpy(p+sizeof code,&fn,8);
    __builtin___clear_cache((char *)p,(char *)p+sizeof code+8);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);uintptr_t begin=(uintptr_t)(g_mod[0].base+0x841540)&~(page-1);
    CHECK(!mprotect((void *)begin,page,PROT_READ|PROT_EXEC));
}
static void bad91738(int kind) {
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid){broken_rebuild=kind;CHECK(!dm_android_render_scale_request(.75f));tick91738();_exit(99);}
    int result;CHECK(waitpid(pid,&result,0)==pid);CHECK(WIFSIGNALED(result)&&WTERMSIG(result)==SIGABRT);
}
int main(void) {
    alarm(20);setup91738();module_t *m=&g_mod[0];CHECK(android_s3_profile_91738(m));CHECK(!android_s3_profile(m));
    /* Manual preference may arrive before pthread_once/env configuration. */
    CHECK(!dm_android_render_scale_request(.75f));CHECK(!setenv("DM_3D_SCALE","1.0",1));
    CHECK(dm_android_render_scale_requested()==.75f);
    char identity[33];strcpy(identity,m->build_id);
    strcpy(m->build_id,"db0507f36b84200ac05f75d76ed5026f");tick91738();CHECK(!writes);strcpy(m->build_id,identity);
    m->build_id[0]='a';tick91738();CHECK(!writes);strcpy(m->build_id,identity);
    m->build_id[0]=0;CHECK(android_s3_profile_91738(m));CHECK(!android_s3_profile(m));strcpy(m->build_id,identity);
    m->text_end--;tick91738();CHECK(!writes);m->text_end++;
    m->ro_off--;tick91738();CHECK(!writes);m->ro_off++;
    m->data_off--;tick91738();CHECK(!writes);m->data_off++;
    m->size=0x31b2537;tick91738();CHECK(!writes);m->size=0x31c0000;
    for(size_t i=0;i<sizeof dm_s3_91738_anchors/sizeof *dm_s3_91738_anchors;i++) {
        uint32_t *word=(uint32_t *)(m->base+dm_s3_91738_anchors[i].address);*word^=1;
        CHECK(!android_s3_profile_91738(m));tick91738();CHECK(!writes);*word^=1;
    }
    android_scale3d_tick(m->base+0xe10d58);CHECK(!writes);
    android_scale3d_tick(m->base+0xdf7bbc);CHECK(!writes);
    frames=29;tick91738();CHECK(!writes);frames=100;
    uintptr_t valid=(uintptr_t)renderer,bad=1;memcpy(m->base+0x31b2528,&bad,8);tick91738();CHECK(!writes);memcpy(m->base+0x31b2528,&valid,8);
    float invalid=NAN;memcpy(renderer+0x66a4,&invalid,4);tick91738();CHECK(!writes);invalid=1;memcpy(renderer+0x66a4,&invalid,4);
    uint32_t *sizes=(uint32_t *)(renderer+0xee78);sizes[4]=960;tick91738();CHECK(!writes);sizes[4]=1280;
    stub91738();m->build_id[0]=0;tick91738();strcpy(m->build_id,identity);
    CHECK(rebuilds==1&&writes==1&&dm_3d_scale_cur()==.75f);CHECK(android_s3_profile_91738(m));
    CHECK(sizes[0]==960&&sizes[1]==540&&sizes[2]==960&&sizes[3]==540&&sizes[4]==1280&&sizes[5]==720);
    CHECK(!dm_android_render_scale_request(1.f));pthread_t thread;
    CHECK(!pthread_create(&thread,NULL,wrong_thread91738,NULL));CHECK(!pthread_join(thread,NULL));CHECK(rebuilds==1);
    tick91738();CHECK(rebuilds==2&&dm_3d_scale_cur()==1.f);
    const uint64_t p60=1000000000ull/60,p30=1000000000ull/30;present(4000,p30,p60);tick91738();
    CHECK(android_s3.level==0&&dm_3d_scale_cur()==1.f);CHECK(sizes[0]==1280&&sizes[1]==720&&sizes[4]==1280&&sizes[5]==720);
    bad91738(1);bad91738(2);
    CHECK(!dm_android_render_scale_request(.75f));tick91738();present(4000,p30,p60);tick91738();
    CHECK(android_s3.level==0&&dm_3d_scale_cur()==.75f);
    android_scale3d_tick(m->base+0xdf7c10);dm_android_scale3d_status status;dm_android_scale3d_status_get(&status);
    CHECK(status.supported&&status.last_caller==0xdf7c10&&status.current==.75f&&status.configured==.75f);
    CHECK(status.output_width==1280&&status.output_height==720&&status.changes==rebuilds);
    /* Even without the manual override, this legacy profile cannot consume
     * the newer engine's .60 dynamic levels or renderer singleton. */
    pthread_mutex_lock(&android_s3_lock);android_s3_manual=0;android_s3_91738_configured=1.f;
    atomic_store(&g_scale3d_level,3);pthread_mutex_unlock(&android_s3_lock);tick91738();
    CHECK(dm_3d_scale_cur()==1.f);present(4000,p30,p60);CHECK(android_s3.level==0);
    printf("ANDROID_D2R_SCALE3D_91738_OK checks=%u rebuilds=%u anchors=12 fixed_1_075=1 manual_before_config=1 wrong_version_addresses_rejected=1 wrong_identity_rejected=1 legacy_empty_id_full_anchors=1 unknown_writes=0 UI_output_unchanged=1 synthetic_renderer=1 actual_game_not_claimed=1\n",checks,rebuilds);
    return 0;
}
