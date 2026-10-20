#define DM_MAP_TEST 1
#include "../d2r_map_reveal.c"
#include <assert.h>
#include <stdarg.h>
static unsigned checks,initialized,revealed;
static uint64_t test_now=1000000000;
static int held,rift,fail_init;
static int hud_area=2;
static uint8_t player[512],path[64],active[64],extra[160],level[512],map[0x840],rooms[20][160];
static dm_android_d2r_scene_status scene={.game=1,.world_epoch=1};
static dm_android_window_state window={.focus=1};
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);abort();}}while(0)
static void ptr(void *base,size_t off,void *value){uint64_t p=(uintptr_t)value;memcpy((char *)base+off,&p,8);}
static void u32(void *base,size_t off,uint32_t value){memcpy((char *)base+off,&value,4);}
uint64_t now_ns(void){return test_now;}
uint32_t dm_hud_held(void){return held;}
void dm_hud_get(dm_hud_info *out){*out=(dm_hud_info){.valid=scene.game,.area=hud_area};}
int dm_rift_active(void){return rift;}
void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *out){*out=scene;}
void dm_android_platform_get_window(dm_android_window_state *out){*out=window;}
static uint64_t map_player(void){return (uintptr_t)player;}
static void map_init_room(int version,uint64_t room){CHECK(version==3);initialized++;if(!fail_init)ptr((void *)(uintptr_t)room,0x58,active);}
static void map_reveal_room(uint64_t callback,uint64_t room){CHECK(callback==(uintptr_t)map_image->base+0x100);CHECK(room==(uintptr_t)active);revealed++;}
static void pump(void){test_now+=16000000;dm_android_map_pump();}
static dm_android_map_status status(void){dm_android_map_status s;dm_android_map_status_get(&s);return s;}
int main(void){
    CHECK(dm_android_map_request(NULL)==EAGAIN);
    module_t module={0};strcpy(module.name,"main");module.base=calloc(1,0x3211700);CHECK(module.base);
    module.size=0x3211700;module.text_end=0x2000000;
    CHECK(!dm_android_map_attach(&module));strcpy(module.build_id,"db0507f36b84200ac05f75d76ed5026f");CHECK(dm_android_map_attach(&module));
    ptr(player,0x38,path);ptr(path,0x20,active);ptr(active,0x18,extra);ptr(extra,0x90,level);
    u32(player,8,5);u32(player,0xc,5);player[0x1bd]=3;u32(level,0x1f8,2);
    ptr(level,0x1c8,map);ptr(map,0x838,module.base+0x100);ptr(level,0x10,rooms[0]);
    for(unsigned i=0;i<20;i++){ptr(rooms[i],0x90,level);ptr(rooms[i],0x48,i<19?rooms[i+1]:NULL);}
    pump();CHECK(status().ready);uint64_t seq=0;CHECK(!dm_android_map_request(&seq)&&seq==1);
    CHECK(dm_android_map_request(NULL)==EBUSY);pump();CHECK(status().state==DM_MAP_RUNNING&&status().done==8);
    pump();CHECK(status().done==16);pump();CHECK(status().state==DM_MAP_DONE&&status().done==20);CHECK(revealed==20);
    pump();CHECK(status().ready);test_now+=600000000;CHECK(dm_android_map_request(NULL)==EAGAIN);pump();
    for(unsigned i=0;i<9;i++){
        CHECK(!dm_android_map_request(NULL));unsigned before=revealed;
        if(i==0)scene.navigation=1;if(i==1)window.focus=3;if(i==2)u32(player,0xc,17);
        if(i==3)held=1;if(i==4)rift=1;if(i==5)module.base[0x2d5ff51]=1;
        if(i==6)scene.world_epoch++;if(i==7)u32(player,8,6);if(i==8)ptr(path,0x20,NULL);
        pump();CHECK(status().state==DM_MAP_CANCELLED);CHECK(revealed==before);
        scene.navigation=0;window.focus=1;u32(player,0xc,5);held=rift=0;module.base[0x2d5ff51]=0;ptr(path,0x20,active);pump();
    }
    ptr(rooms[19],0x48,rooms[0]);CHECK(!dm_android_map_request(NULL));pump();CHECK(status().state==DM_MAP_FAILED);ptr(rooms[19],0x48,NULL);pump();
    ptr(map,0x838,module.base+module.text_end);pump();CHECK(!status().ready);CHECK(dm_android_map_request(NULL)==EAGAIN);
    ptr(map,0x838,module.base+0x100);pump();CHECK(!dm_android_map_request(NULL));
    unsigned before=revealed;test_now+=31000000000ull;pump();CHECK(status().error==ETIMEDOUT&&revealed==before);pump();
    fail_init=1;ptr(rooms[0],0x58,NULL);CHECK(!dm_android_map_request(NULL));pump();CHECK(status().state==DM_MAP_FAILED&&status().error==EIO);fail_init=0;pump();
    CHECK(!dm_android_map_request(NULL));pump();CHECK(status().done==8);scene.world_epoch++;pump();CHECK(status().state==DM_MAP_CANCELLED&&status().done==8);
    /* 设置页中仅排队，解除暂停后执行；换区、退出和超时撤销。
     * Options enqueue only; unpause to execute, or cancel on area change, exit or timeout. */
    scene.navigation=1;module.base[0x2d5ff51]=1;pump();before=revealed;
    CHECK(!status().ready&&status().game);CHECK(dm_android_map_request(NULL)==EAGAIN);
    CHECK(!dm_android_map_request_when_ready(&seq));CHECK(status().state==DM_MAP_WAITING);
    CHECK(dm_android_map_request_when_ready(NULL)==EBUSY&&dm_android_map_request(NULL)==EBUSY);
    pump();CHECK(status().state==DM_MAP_WAITING&&revealed==before);
    scene.navigation=0;pump();CHECK(status().state==DM_MAP_WAITING&&revealed==before);
    module.base[0x2d5ff51]=0;window.focus=3;pump();CHECK(status().state==DM_MAP_WAITING&&revealed==before);
    window.focus=1;pump();CHECK(status().state==DM_MAP_RUNNING&&status().done==8&&status().area==2);
    pump();pump();CHECK(status().state==DM_MAP_DONE&&revealed==before+20);pump();
    scene.navigation=1;pump();CHECK(status().state==DM_MAP_DONE&&status().done==20);
    scene.navigation=0;pump();CHECK(status().state==DM_MAP_DONE);
    for(unsigned i=0;i<4;i++) {
        scene.navigation=1;module.base[0x2d5ff51]=1;pump();before=revealed;
        CHECK(!dm_android_map_request_when_ready(NULL));
        if(i==0)scene.world_epoch++;if(i==1)scene.game=0;if(i==2)test_now+=31000000000ull;
        if(i==3)hud_area=3;
        pump();CHECK(status().state==(i==2?DM_MAP_FAILED:DM_MAP_CANCELLED)&&revealed==before);
        CHECK(status().error==(i==2?ETIMEDOUT:ECANCELED));
        if(i==1)CHECK(dm_android_map_request_when_ready(NULL)==EAGAIN);
        hud_area=2;scene.game=1;scene.navigation=0;module.base[0x2d5ff51]=0;pump();
    }
    test_now+=600000000;CHECK(dm_android_map_request_when_ready(NULL)==EAGAIN);
    printf("{\"passed\":true,\"checks\":%u,\"original_game_executed\":false}\n",checks);
    free(module.base);return 0;
}
