/* Host-only behavioral fixtures; original guest functions are mocks. */
#define DM_DEBUG_TEST 1
#include "../d2r_debug_mode.c"
#include <assert.h>
#include <stdarg.h>

static uint64_t clock_ns=1000000000ull;
static dm_android_d2r_scene_status scene={.game=1,.world_epoch=1};
static dm_android_window_state window={.focus=1};
static uint32_t held;
static int rift, calls, drops, pickup_calls, warps, fail_drop, full_bag, merge_item;
static int stats[16];
static unsigned serial, checks;
static uint8_t game_mem[0x19100], player_mem[512], path_mem[64], room_mem[64], extra_mem[160], level_mem[512];
static uint8_t table_mem[0x1600], rows_mem[0x1c0*2], item_mem[512], item_path[64];
static module_t module;
#define CHECK(x) do {checks++;if(!(x)){fprintf(stderr,"check %u at %d: %s\n",checks,__LINE__,#x);abort();}} while(0)
static void u32(void *p,size_t off,uint32_t v){memcpy((char *)p+off,&v,4);}
static void ptr(void *p,size_t off,void *v){uint64_t n=(uintptr_t)v;memcpy((char *)p+off,&n,8);}
uint64_t now_ns(void){return clock_ns;}
int gate_obs_present(int gate){return gate==OBS_TICK;}
void dm_guest_write_prepare(void *p,size_t n){CHECK(p && n==4);}
void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *out){*out=scene;}
void dm_android_platform_get_window(dm_android_window_state *out){*out=window;}
uint32_t dm_hud_held(void){return held;}
int dm_rift_active(void){return rift;}
uint64_t dm_diag_srv_item(uint32_t id){CHECK(id==77);return merge_item?0:(uintptr_t)item_mem;}
static uint64_t drop(uint64_t g,uint64_t p,uint32_t code,int quality){
    CHECK(g==(uintptr_t)game_mem && p==(uintptr_t)player_mem);
    CHECK(code==0x20303372 || code==0x20706d68);CHECK(quality>=0 && quality<=7);
    calls++;drops++;u32(item_mem,0xc,3);return fail_drop?0:(uintptr_t)item_mem;
}
static void dispatch(uint64_t g,uint64_t p,uint8_t *packet){
    CHECK(g==(uintptr_t)game_mem && p==(uintptr_t)player_mem);
    uint32_t one=0;memcpy(&one,game_mem+0x19050,4);CHECK(one==1);
    uint8_t expected[17]={0x16,77,0,0,0,40,0,0,0,50,0,0,0,1,0,0,0};
    CHECK(!memcmp(packet,expected,17));pickup_calls++;calls++;
    if(!full_bag)u32(item_mem,0xc,0);
}
static void warp(uint64_t g,uint64_t p,int level){CHECK(g==(uintptr_t)game_mem && p==(uintptr_t)player_mem && level==40);warps++;calls++;}
static int get_stat(uint64_t p,int stat){CHECK(p==(uintptr_t)player_mem && stat<16);calls++;return stats[stat];}
static void set_stat(uint64_t p,int stat,int value){CHECK(p==(uintptr_t)player_mem && stat<16);calls++;stats[stat]=value;}
static void poll_tick(uint64_t runs){clock_ns+=300000000;u32(game_mem,0x170,serial+30);dm_android_d2r_debug_tick((uintptr_t)game_mem,(uintptr_t)player_mem,module.base,runs);}
static void raw(const char *text,size_t n){
    char p[PATH_MAX];CHECK(path_join(p,sizeof p,dbg.directory,"request.tmp"));
    FILE *f=fopen(p,"wb");CHECK(f);CHECK(fwrite(text,1,n,f)==n);CHECK(!fclose(f));CHECK(!rename(p,dbg.inbox));
}
static void cmd(const char *text){
    char line[1024];snprintf(line,sizeof line,"r%u %llx %s\n",++serial,(unsigned long long)dbg.token,text);raw(line,strlen(line));poll_tick(1);
    CHECK(access(dbg.inbox,F_OK)!=0 && access(dbg.claimed,F_OK)!=0);
}
static void state(const char *s){CHECK(!strcmp(dbg.outcome,s));}
int main(void){
    char root[]="/tmp/d2r-debug-test-XXXXXX";CHECK(mkdtemp(root));char save[PATH_MAX]="normal-save";
    unsetenv("DM_DEBUG_MODE");CHECK(!dm_android_d2r_debug_configure(root,save,sizeof save));CHECK(!dbg.enabled && !strcmp(save,"normal-save"));
    setenv("DM_DEBUG_MODE","false",1);CHECK(!dm_android_d2r_debug_configure(root,save,sizeof save));CHECK(!dbg.enabled);
    setenv("DM_DEBUG_MODE","1",1);CHECK(!dm_android_d2r_debug_configure(root,save,sizeof save));CHECK(dbg.enabled && strstr(save,"/debug/save"));
    module.base=calloc(1,0x3211700);CHECK(module.base);module.size=0x3211700;strcpy(module.name,"main");
    CHECK(!dm_android_d2r_debug_attach(&module));state("unsupported");strcpy(module.build_id,D2R_BUILD_ID);
    CHECK(dm_android_d2r_debug_attach(&module));
    ptr(player_mem,0x38,path_mem);ptr(path_mem,0x20,room_mem);ptr(room_mem,0x18,extra_mem);ptr(extra_mem,0x90,level_mem);
    u32(level_mem,0x1f8,1);u32(player_mem,8,123);u32(player_mem,0xc,5);player_mem[0x1bd]=3;
    ptr(module.base,0x2d64a18+3*16,table_mem);ptr(table_mem,0x15c0,rows_mem);u32(table_mem,0x15c8,2);
    u32(rows_mem,0x80,0x20303372);u32(rows_mem,0x1c0+0x80,0x20706d68);
    ptr(item_mem,0x38,item_path);u32(item_mem,8,77);u32(item_path,0x10,40);u32(item_path,0x14,50);
    stats[7]=12800;stats[9]=25600;stats[12]=10;
    poll_tick(1);CHECK(dbg.ready);cmd("status");state("ok");CHECK(calls==0);
    const char *bad[]={"give", "give toolong", "give BAD", "give r30 -1", "give r30 1", "give r30 3", "give r30 8", "give r30 2 0", "give r30 2 17", "give r30 2 9999999999999999999999999999999", "give r30 2 1 inv extra", "give r30 inv 2", "scene 0", "scene 137", "scene 199", "scene -1", "scene 40 suffix", "heal extra", "gold -1", "gold 100001", "gold 0x10", "gold 1 suffix", "nope"};
    for(unsigned i=0;i<sizeof bad/sizeof *bad;i++){int d=drops,w=warps;cmd(bad[i]);state("rejected");CHECK(d==drops && w==warps);}
    cmd("give r30 2 2 inventory");state("ok");CHECK(dbg.made==2 && dbg.picked==2 && pickup_calls==2);
    uint32_t flag;memcpy(&flag,game_mem+0x19050,4);CHECK(!flag);
    int before=calls;char retry[128];snprintf(retry,sizeof retry,"%s %llx give r30 2 2 inventory",dbg.request,(unsigned long long)dbg.token);raw(retry,strlen(retry));poll_tick(1);CHECK(calls==before);
    full_bag=1;cmd("give r30 inventory");state("partial");CHECK(dbg.made==1 && dbg.picked==0);full_bag=0;
    merge_item=1;cmd("give r30 inv");state("partial");merge_item=0;
    fail_drop=1;cmd("give r30");state("failed");fail_drop=0;
    u32(game_mem,0x19050,9);before=pickup_calls;cmd("give r30 inv");state("partial");CHECK(pickup_calls==before);memcpy(&flag,game_mem+0x19050,4);CHECK(flag==9);u32(game_mem,0x19050,0);
    cmd("heal");state("ok");CHECK(stats[6]==12800 && stats[8]==25600);
    cmd("gold 100000");state("ok");CHECK(stats[14]==100000);
    /* A receipt failure must prevent mutations and leave a truthful result. */
    char status_temp[PATH_MAX];CHECK(path_join(status_temp,sizeof status_temp,dbg.directory,"status.tmp"));
    CHECK(!mkdir(status_temp,0700));
    before=drops;cmd("give r30");state("io_error");CHECK(drops==before);
    stats[6]=100;stats[8]=200;cmd("heal");state("io_error");CHECK(stats[6]==100 && stats[8]==200);
    cmd("gold 123");state("io_error");CHECK(stats[14]==100000);
    before=warps;cmd("scene lut");state("io_error");CHECK(warps==before && !dbg.target);
    CHECK(!rmdir(status_temp));poll_tick(1);CHECK(dbg.ready);
    for(int i=0;i<7;i++){
        if(i==0)scene.navigation=1;if(i==1)window.focus=3;if(i==2)u32(player_mem,0xc,17);if(i==3)module.base[0x2d5ff51]=1;
        if(i==4)held=1;if(i==5)rift=1;if(i==6)ptr(path_mem,0x20,NULL);
        before=calls;cmd("give r30");state("rejected");CHECK(calls==before);
        scene.navigation=0;window.focus=1;u32(player_mem,0xc,5);module.base[0x2d5ff51]=0;held=0;rift=0;ptr(path_mem,0x20,room_mem);
    }
    poll_tick(1);char stale[128];snprintf(stale,sizeof stale,"old %llx give r30",(unsigned long long)dbg.token);scene.world_epoch++;poll_tick(1);before=calls;raw(stale,strlen(stale));poll_tick(1);state("rejected");CHECK(calls==before);
    raw("bad 1 status\nheal",17);poll_tick(1);state("rejected");CHECK(calls==before);
    char longline[300];memset(longline,'x',sizeof longline);raw(longline,sizeof longline);poll_tick(1);state("rejected");
    const char nul[]="id 1 status\0ignored";raw(nul,sizeof nul);poll_tick(1);state("rejected");
    CHECK(!mkfifo(dbg.inbox,0600));poll_tick(1);state("rejected");
    CHECK(!symlink("/dev/zero",dbg.inbox));poll_tick(1);state("rejected");
    cmd("scene lut");state("transition");CHECK(warps==1 && !dbg.ready && dbg.target==40);
    poll_tick(1);state("transition");CHECK(warps==1);
    scene.navigation=1;u32(level_mem,0x1f8,40);poll_tick(1);state("transition");
    scene.navigation=0;poll_tick(1);state("ok");CHECK(dbg.ready && !dbg.target);
    u32(level_mem,0x1f8,1);poll_tick(1);cmd("scene 40");clock_ns+=31000000000ull;poll_tick(1);state("timeout");CHECK(warps==2);
    cmd("scene 40");poll_tick(2);state("cancelled");CHECK(warps==3);
    /* JSON is parsed again by the Python runner. */
    printf("{\"passed\":true,\"checks\":%u,\"receipt\":\"%s\",\"original_game_executed\":false}\n",checks,dbg.status);
    free(module.base);return 0;
}
