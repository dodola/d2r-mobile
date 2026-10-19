/* Standalone title-adapter fixture. Its heap/NSO-shaped data is synthetic;
 * only the production observer, checked memory access, lifecycle waiter and
 * real HLE transaction implementation are under test. No APK/save is used. */
#include "../d2r_save_adapter.h"
#include "../lifecycle_android.h"
#include "../services_fs.h"
#include "../../host/dm_hud.h"
#include "../../host/hle.h"
#include <errno.h>
#include <limits.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <unistd.h>
#ifdef DM_ANDROID_HUD_EXP_HEADER
#include DM_ANDROID_HUD_EXP_HEADER
#endif

struct dm_flags g_flag;
uint64_t g_t0_ns;
static _Atomic unsigned checks;
extern _Atomic int g_dm_frz_want;
#define CHECK(expression) do { ++checks; if (!(expression)) die("D2R save fixture failed line=%d", __LINE__); } while (0)
static void store(void *base, size_t offset, const void *value, size_t bytes) {
    memcpy((uint8_t *)base + offset, value, bytes);
}
static void u32(void *base, size_t offset, uint32_t value) { store(base, offset, &value, 4); }
static void u64(void *base, size_t offset, uint64_t value) { store(base, offset, &value, 8); }
static void pointer(void *base, size_t offset, const void *value) { u64(base, offset, (uintptr_t)value); }
static void *block(size_t bytes) { void *p=calloc(1,bytes); CHECK(p); return p; }
extern void gate_register(uint64_t, const char *, int, uint64_t);
static void tick(void *game) {
    uint64_t registers[31]={0}; registers[0]=(uintptr_t)game;
    gate_dispatch(0x10000,registers);
}
static void stats(void *unit, void *list, uint32_t (*pairs)[2], size_t count) {
    pointer(unit,0x88,list); pointer(list,0x30,pairs); u64(list,0x38,count);
}
static void *binding(const char *name) {
    extern const hle_force_ent __start_dm_force[], __stop_dm_force[];
    for (const hle_force_ent *e=__start_dm_force; e<__stop_dm_force; ++e)
        if (!strcmp(e->name,name)) return e->fn;
    die("D2R save fixture missing binding %s",name);
}
static uint8_t *game;
static uint8_t *main_image;
static _Atomic int fixture_players;
int dm_android_d2r_players_get(uint8_t *base) {
    CHECK(base==main_image); return atomic_load(&fixture_players);
}
static Result (*commit_save)(const char *);
static Result committed;
static void *save_worker(void *unused) {
    (void)unused;
    uint64_t deadline=now_ns()+1000000000;
    while (now_ns()<deadline) {
        tick(game);
        uint64_t value; memcpy(&value,game+0x5800,8);
        if (value==1) { committed=commit_save("adapter_fixture"); return NULL; }
        usleep(1000);
    }
    return NULL;
}
static void observe(unsigned id,uint64_t *registers) { gate_dispatch(0x10000+id*0x10,registers); }
static void freeze_contract(int want) {
    uint64_t r[31]={0};
    r[0]=(uintptr_t)game; observe(OBS_FREEZE_WORLD,r);
    CHECK((r[0]!=(uintptr_t)game)==want);
    if (want) {
        uint64_t queue=0; uint32_t tick=1;
        memcpy(&queue,(uint8_t *)(uintptr_t)r[0]+0x188,8);
        memcpy(&tick,(uint8_t *)(uintptr_t)r[0]+0x170,4);
        CHECK(queue && !tick);
        for (unsigned i=0;i<0x1500/8;++i) CHECK(!((uint64_t *)(uintptr_t)queue)[i]);
    }
    r[0]=(uintptr_t)game; observe(OBS_FREEZE_CLIENT,r);
    CHECK((r[0]!=(uintptr_t)game)==want);
    if (want) CHECK(*(uint32_t *)((uint8_t *)(uintptr_t)r[0]+0x124)==(1u<<23));
    r[0]=(uintptr_t)game; observe(OBS_FREEZE_ANIM,r);
    CHECK((r[0]!=(uintptr_t)game)==want);
    if (want) CHECK(*(uint32_t *)(uintptr_t)r[0]==9);
    r[0]=0; observe(OBS_FREEZE_CLIENT,r); CHECK(!r[0]);
    observe(OBS_FREEZE_ANIM,r); CHECK(!r[0]);
}
static _Atomic int paused_worker_mode;
static _Atomic unsigned frozen_ticks;
static int pause_active(void) {
    return __atomic_load_n(main_image+0x2d5ff51,__ATOMIC_ACQUIRE) ||
        __atomic_load_n(main_image+0x2d5ff67,__ATOMIC_ACQUIRE);
}
static void frozen_render(void) {
    uint64_t r[31]={0};
    u32(main_image,0x20cebb8,27); u64(main_image,0x3114208,44444);
    observe(OBS_FREEZE_RENDER,r);
    uint32_t step; uint64_t time;
    memcpy(&step,main_image+0x20cebb8,4); memcpy(&time,main_image+0x3114208,8);
    CHECK(!step && time==12345);
}
static void simulate_frozen_tick(uint32_t initial) {
    tick(game);
    uint32_t current; memcpy(&current,game+0x170,4);
    CHECK(current==initial-1); u32(game,0x170,current+1); /* original tick body */
    uint64_t r[31]={0}; r[0]=(uintptr_t)game; observe(OBS_FREEZE_WORLD,r);
    CHECK(r[0]!=(uintptr_t)game);
    frozen_render();
}
static void *paused_worker(void *unused) {
    (void)unused;
    uint64_t deadline=now_ns()+1000000000;
    while (now_ns()<deadline && pause_active()) usleep(1000);
    CHECK(!pause_active());
    freeze_contract(1);
    int mode=atomic_load(&paused_worker_mode);
    if (mode==1) {
        /* Cancel between OBS_TICK and the original world body, then emulate
         * a scheduler that passed its pause check before rollback. */
        tick(game); uint32_t current; memcpy(&current,game+0x170,4);
        CHECK(current==99); dm_android_save_cancel();
        CHECK(pause_active()); u32(game,0x170,current+1);
        freeze_contract(1); frozen_render(); simulate_frozen_tick(100);
        return NULL;
    }
    if (mode==2) {
        u64(main_image,0x2d665d8,999);
        main_image[0x2d5ff51]=7; main_image[0x2d5ff67]=9;
        CHECK(!dm_ingame_now()); return NULL;
    }
    if (mode==3) {
        size_t page=(size_t)sysconf(_SC_PAGESIZE);
        uintptr_t pause_page=((uintptr_t)main_image+0x2d5ff51)&~(page-1);
        CHECK(!mprotect((void *)pause_page,page,PROT_READ));
        dm_android_save_cancel(); freeze_contract(1);
        CHECK(!mprotect((void *)pause_page,page,PROT_READ|PROT_WRITE));
        dm_android_save_cancel(); return NULL;
    }
    uint32_t initial; memcpy(&initial,game+0x170,4);
    int saved=0;
    while (now_ns()<deadline && !pause_active()) {
        simulate_frozen_tick(initial);
        uint64_t timer; memcpy(&timer,game+0x5800,8);
        if (!saved && timer==1) { committed=commit_save("adapter_fixture"); saved=1; }
        atomic_fetch_add(&frozen_ticks,1); usleep(1000);
    }
    CHECK(saved && pause_active());
    /* A tick already admitted by the scheduler must remain frozen even if
     * its observer only runs after the pause bytes have been restored. */
    simulate_frozen_tick(initial); freeze_contract(1);
    return NULL;
}
static void prepare_paused(unsigned mode,uint64_t run) {
    dm_android_save_cancel();
    main_image[0x2d5ff51]=main_image[0x2d5ff67]=0;
    u64(main_image,0x2d665d8,run); u32(game,0x170,100); u64(game,0x5800,123456);
    tick(game); freeze_contract(0);
    u32(main_image,0x20cebb8,27); u64(main_image,0x3114208,12345);
    main_image[0x2d5ff51]=2; main_image[0x2d5ff67]=3;
    committed=UINT32_MAX; atomic_store(&paused_worker_mode,mode);
    atomic_store(&frozen_ticks,0);
}
static void verify_resumed(void) {
    CHECK(main_image[0x2d5ff51]==2 && main_image[0x2d5ff67]==3);
    main_image[0x2d5ff51]=main_image[0x2d5ff67]=0;
    tick(game); freeze_contract(0);
    uint32_t current; memcpy(&current,game+0x170,4); CHECK(current==100);
    /* The genuine resumed tick body now advances, without a decrement. */
    u32(game,0x170,current+1); tick(game);
    memcpy(&current,game+0x170,4); CHECK(current==101);
}
int main(int argc, char **argv) {
    if (argc!=3 || argv[1][0]!='/') return 2;
    g_t0_ns=now_ns();
    module_t image={0}; strcpy(image.name,"main"); image.size=0x3120000;
    image.base=mmap(NULL,image.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(image.base!=MAP_FAILED);
    main_image=image.base;
    CHECK(!dm_android_d2r_save_attach(NULL));
    CHECK(!dm_android_d2r_save_attach(&image));
    strcpy(image.build_id,"db0507f36b84200ac05f75d76ed5026f");
    CHECK(!dm_android_d2r_save_attach(&image)); /* no observer */
    gate_register(0x10000,"obs",OBS_TICK,0);
    image.build_id[0]='a'; CHECK(!dm_android_d2r_save_attach(&image));
    image.build_id[0]='d';
    size_t full=image.size; image.size=0x1000;
    CHECK(!dm_android_d2r_save_attach(&image)); image.size=full;
    CHECK(dm_android_d2r_save_attach(&image));
    CHECK(dm_android_d2r_save_attach(&image));
    CHECK(dm_android_save_adapter_available() && !dm_ingame_now() && !dm_request_save());

    const size_t game_bytes=0x10000;
    game=mmap(NULL,game_bytes,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(game!=MAP_FAILED);
    u64(image.base,0x2d665d8,17); u32(game,0x170,100);
    void *player=block(0x160), *corpse=block(0x160), *inventory=block(0x100);
    void *path=block(0x50), *room=block(0x30), *area_room=block(0xa0), *area=block(0x210);
    void *list=block(0x110);
    uint32_t base[][2]={{12u<<16,10},{13u<<16,10000},{4u<<16,5},{5u<<16,2},
        {6u<<16,256},{7u<<16,512},{8u<<16,768},{9u<<16,1024}};
    uint32_t totals[][2]={{6u<<16,8*256+128},{7u<<16,17*256},{8u<<16,23*256},{9u<<16,40*256}};
    u32(image.base,0x2d20e4c,0); u32(image.base,0x2d60380,5);
    u32(player,8,5); u32(player,0xc,1); pointer(player,0x38,path); pointer(player,0x90,inventory);
    u32(corpse,8,6); u32(corpse,0xc,17); pointer(corpse,0x158,player);
    pointer(game,0x2230+5*8,corpse); u32(inventory,0,0x01020304);
    u32(path,0,5010u<<16); u32(path,4,5100u<<16); pointer(path,0x20,room);
    pointer(room,0x18,area_room); pointer(area_room,0x90,area); u32(area,0x1f8,40);
    stats(player,list,base,sizeof base/sizeof *base);
    u32(list,0x1c,0x80000000u); pointer(list,0xe8,totals); u64(list,0xf0,4);

    void *potion=block(0x160), *scroll=block(0x160), *tome=block(0x160);
    void *pd=block(0xc0), *sd=block(0xc0), *td=block(0xc0), *pp=block(0x30), *ts=block(0x110);
    uint32_t quantity[][2]={{70u<<16,6}};
    u32(potion,0,4); u32(potion,0xc,2); pointer(potion,0x10,pd); pointer(potion,0x38,pp); u32(pp,0x10,5);
    u32(scroll,0,4); u32(scroll,4,544); pointer(scroll,0x10,sd);
    u32(tome,0,4); u32(tome,4,533); pointer(tome,0x10,td); stats(tome,ts,quantity,1);
    pointer(inventory,0x10,potion); pointer(pd,0xb0,scroll); pointer(sd,0xb0,tome);
    void *client=block(0x160), *client_inventory=block(0x100), *held=block(0x160);
    u32(client,8,5); pointer(client,0x90,client_inventory);
    pointer(image.base,0x2d603a0+5*8,client); u32(client_inventory,0,0x01020304);
    pointer(client_inventory,0x40,held); u32(held,0,4); u32(held,8,35);
    pointer(image.base,0x2d603a0+4*0x400+35*8,held);
    /* Server cursor differs: only the verified client cursor counts. */
    pointer(inventory,0x40,scroll);
    image.base[0x2d5ff58]=1;
    tick(game);
    CHECK(dm_ingame_now());
    uint64_t context_game=0,context_player=0;
    CHECK(dm_android_d2r_server_context(&context_game,&context_player) &&
          context_game==(uintptr_t)game && context_player==(uintptr_t)player);
    CHECK(dm_cur_tick()==100 && dm_diag_difficulty()==0);
    game[0x104]=2; CHECK(dm_diag_difficulty()==2);
    game[0x104]=3; CHECK(dm_diag_difficulty()==-1); game[0x104]=0;
    void *server_item=block(0x160), *merc=block(0x160), *merc_stats=block(0xb50);
    uint32_t merc_values[][2]={{6u<<16,22*256+128},{7u<<16,35*256}};
    u32(server_item,0,4); u32(server_item,8,35);
    pointer(game,0x2e30+35*8,server_item);
    CHECK(dm_diag_srv_item(35)==(uintptr_t)server_item && !dm_diag_srv_item(36));
    u32(server_item,0,2); pointer(server_item,0x158,server_item);
    CHECK(!dm_diag_srv_item(35)); u32(server_item,0,4); pointer(server_item,0x158,NULL);
    u32(merc,0,1); u32(merc,8,117); u32(merc,0xc,1);
    pointer(game,0x2630+117*8,merc); stats(merc,merc_stats,merc_values,2);
    u32(merc_stats,0x1c,0x80000000); pointer(merc_stats,0xe8,merc_values); u64(merc_stats,0xf0,2);
    int hp=-1,mx=-1,healing=-1;
    CHECK(dm_diag_srv_merc(117,&hp,&mx,&healing) && hp==22 && mx==35 && !healing);
    u32(merc_stats,0xb30+4*(100>>5),1u<<(100&31));
    CHECK(dm_diag_srv_merc(117,&hp,&mx,&healing) && healing);
    u32(merc,0xc,12); CHECK(!dm_diag_srv_merc(117,&hp,&mx,&healing));
    u32(merc,0xc,0); CHECK(!dm_diag_srv_merc(117,&hp,&mx,&healing)); u32(merc,0xc,1);
    CHECK(!dm_diag_srv_merc(118,&hp,&mx,&healing) && !dm_diag_srv_merc(117,NULL,&mx,&healing));
    dm_hud_info hud; dm_hud_get(&hud);
    CHECK(hud.valid && hud.level==10 && hud.area==40 && hud.x==5010 && hud.y==5100);
    CHECK(hud.hp==8 && hud.hp_max==17 && hud.mp==23 && hud.mp_max==40);
    CHECK(hud.points==7 && hud.quest_unread && !hud.dead && hud.belt[1]==1 && hud.portal==7);
    CHECK(!hud.players);
    atomic_store(&fixture_players,8); tick(game); dm_hud_get(&hud); CHECK(hud.players==8);
    atomic_store(&fixture_players,9); tick(game); dm_hud_get(&hud); CHECK(!hud.players);
    atomic_store(&fixture_players,-1); tick(game); dm_hud_get(&hud); CHECK(!hud.players);
    atomic_store(&fixture_players,1); tick(game); dm_hud_get(&hud); CHECK(hud.players==1);
#ifdef DM_ANDROID_HUD_EXP_HEADER
    /* Level 98 XP exceeds INT_MAX. Check both endpoints and a midpoint
     * against the original-derived table without narrowing its counters. */
    base[0][1]=98; base[1][1]=k_exp[97]; tick(game); dm_hud_get(&hud);
    CHECK(hud.valid && hud.level==98 && hud.exp_permille==0);
    base[1][1]=k_exp[97]+(k_exp[98]-k_exp[97])/2; tick(game); dm_hud_get(&hud);
    CHECK(hud.exp_permille>=499 && hud.exp_permille<=500);
    base[1][1]=k_exp[98]; tick(game); dm_hud_get(&hud); CHECK(hud.exp_permille==1000);
    base[0][1]=99; tick(game); dm_hud_get(&hud); CHECK(hud.exp_permille==1000);
    base[0][1]=10; base[1][1]=10000; tick(game); dm_hud_get(&hud);
#endif
    CHECK(dm_hud_held()==35 && hud.holding);
    u32(held,0,1); tick(game); CHECK(!dm_hud_held()); u32(held,0,4);
    pointer(image.base,0x2d603a0+4*0x400+35*8,scroll);
    tick(game); CHECK(!dm_hud_held());
    pointer(image.base,0x2d603a0+4*0x400+35*8,held);
    u32(client_inventory,0,0); tick(game); CHECK(!dm_hud_held());
    u32(client_inventory,0,0x01020304); tick(game); CHECK(dm_hud_held()==35);
    pointer(client_inventory,0x40,(void *)(uintptr_t)(UINT64_MAX-8));
    tick(game); CHECK(!dm_hud_held());
    pointer(client_inventory,0x40,held); tick(game); CHECK(dm_hud_held()==35);
    CHECK(!dm_request_save()); /* first tick has no autosave timer yet */
    uint64_t timer=123456; u64(game,0x5800,timer);
    image.base[0x2d5ff51]=1;
    CHECK(!dm_request_save()); CHECK(game[0x5800]==(uint8_t)timer && image.base[0x2d5ff51]==1);
    uint64_t no_commits=0; int no_freeze=-1; double no_ms=0;
    CHECK(!dm_save_wait(.04,&no_commits,&no_freeze,&no_ms) && !no_commits && !no_freeze);
    CHECK(image.base[0x2d5ff51]==1); /* absent freeze gates fail closed */
    image.base[0x2d5ff51]=0; image.base[0x2d5ff67]=1;
    CHECK(!dm_request_save() && image.base[0x2d5ff67]==1);
    image.base[0x2d5ff67]=0;
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    uintptr_t save_page=((uintptr_t)game+0x5800)&~(page-1);
    CHECK(!mprotect((void *)save_page,page,PROT_READ));
    CHECK(dm_request_save()); tick(game);
    uint64_t saved=0; memcpy(&saved,game+0x5800,8); CHECK(saved==timer);
    CHECK(!mprotect((void *)save_page,page,PROT_READ|PROT_WRITE));
    CHECK(dm_request_save()); tick(game); memcpy(&saved,game+0x5800,8); CHECK(saved==1);
    u32(player,0xc,0); tick(game); dm_hud_get(&hud); CHECK(hud.valid && hud.dead);
    u32(player,0xc,17); tick(game); dm_hud_get(&hud); CHECK(hud.valid && hud.dead);
    u32(image.base,0x2d60380,999); tick(game); dm_hud_get(&hud); CHECK(!hud.valid && !dm_hud_held());
    u32(image.base,0x2d60380,5); u32(player,0xc,1); tick(game);
    u64(game,0x5800,timer); CHECK(dm_request_save());
    u64(image.base,0x2d665d8,18); CHECK(!dm_ingame_now() && !dm_request_save());
    CHECK(!dm_android_d2r_server_context(&context_game,&context_player) && !context_game && !context_player);
    CHECK(!dm_cur_tick() && dm_diag_difficulty()==-1 && !dm_diag_srv_item(35) &&
          !dm_diag_srv_merc(117,&hp,&mx,&healing));
    dm_hud_get(&hud); CHECK(!hud.valid);
    tick(game); CHECK(dm_ingame_now());
    memcpy(&saved,game+0x5800,8); CHECK(saved==timer);
    u64(image.base,0x2d665d8,UINT64_MAX); CHECK(!dm_ingame_now());
    u64(image.base,0x2d665d8,18); tick((void *)(uintptr_t)(UINT64_MAX-8)); CHECK(!dm_ingame_now());
    tick(game); CHECK(dm_ingame_now());

    char save[PATH_MAX], rom[PATH_MAX], temp[PATH_MAX];
    CHECK(snprintf(save,sizeof save,"%s/d2r-adapter-save-%d",argv[1],getpid())<(int)sizeof save);
    CHECK(snprintf(rom,sizeof rom,"%s/rom",save)<(int)sizeof rom);
    CHECK(snprintf(temp,sizeof temp,"%s/temp",save)<(int)sizeof temp);
    CHECK(!dm_android_fs_ensure_directory(save) && !dm_android_fs_ensure_directory(rom) &&
          !dm_android_fs_ensure_directory(temp));
    CHECK(!setenv("DM_SAVE",save,1) && !setenv("DM_ROMFS",rom,1) && !setenv("DM_TEMP",temp,1));
    Result (*ensure)(const void *)=binding("_ZN2nn2fs14EnsureSaveDataERKNS_7account3UidE");
    Result (*mount)(const char *,const void *)=binding("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
    Result (*create)(const char *,int64_t)=binding("_ZN2nn2fs10CreateFileEPKcl");
    commit_save=binding("_ZN2nn2fs6CommitEPKc");
    CHECK(!ensure(NULL) && !mount("adapter_fixture",NULL));
    CHECK(!create("adapter_fixture:/fixture.bin",16));
    u64(game,0x5800,timer); committed=UINT32_MAX;
    pthread_t worker; CHECK(!pthread_create(&worker,NULL,save_worker,NULL));
    uint64_t commits=0; int froze=-1; double ms=0;
    CHECK(dm_save_wait(1,&commits,&froze,&ms));
    CHECK(!pthread_join(worker,NULL));
    CHECK(!committed && commits==1 && !froze && ms>=200 && ms<1500);

    for (unsigned id=OBS_FREEZE_WORLD;id<OBS_FREEZE_RENDER;++id)
        gate_register(0x10000+id*0x10,"obs",id,0);
    image.base[0x2d5ff51]=2; image.base[0x2d5ff67]=3;
    CHECK(!dm_save_wait(.01,&commits,&froze,&ms) && !commits && !froze);
    CHECK(image.base[0x2d5ff51]==2 && image.base[0x2d5ff67]==3);
    gate_register(0x10000+OBS_FREEZE_RENDER*0x10,"obs",OBS_FREEZE_RENDER,0);

    /* Panel freezing remains opt-in, delays periodic ticks, and releases
     * after ten presented frames when the original pause stopped ticks. */
    image.base[0x2d5ff51]=image.base[0x2d5ff67]=0;
    u32(game,0x170,101); u64(image.base,0x3114208,12345);
    atomic_store(&g_dm_frz_want,1); tick(game); freeze_contract(0); /* default off */
    CHECK(!setenv("DM_PANEL_FREEZE","1",1));
    u32(game,0x170,100); tick(game); freeze_contract(0); /* periodic tick */
    u32(game,0x170,101); simulate_frozen_tick(101); freeze_contract(1);
    atomic_store(&g_dm_frz_want,0);
    for (unsigned i=0;i<9;++i) dm_freeze_frame(0);
    uint64_t panel_regs[31]={0}; panel_regs[0]=(uintptr_t)game;
    observe(OBS_FREEZE_CLIENT,panel_regs); CHECK(panel_regs[0]!=(uintptr_t)game);
    dm_freeze_frame(0); panel_regs[0]=(uintptr_t)game;
    observe(OBS_FREEZE_CLIENT,panel_regs); CHECK(panel_regs[0]==(uintptr_t)game);
    tick(game); freeze_contract(0); CHECK(!unsetenv("DM_PANEL_FREEZE"));

    CHECK(!create("adapter_fixture:/paused.bin",16));
    prepare_paused(0,19);
    CHECK(!pthread_create(&worker,NULL,paused_worker,NULL));
    CHECK(dm_save_wait(1,&commits,&froze,&ms)); CHECK(!pthread_join(worker,NULL));
    CHECK(!committed && commits==1 && froze && ms>=200 && ms<1500);
    CHECK(atomic_load(&frozen_ticks)>1); verify_resumed();

    prepare_paused(0,20);
    CHECK(dm_save_wait(.05,&commits,&froze,&ms) && !commits && froze);
    CHECK(ms>=40 && ms<500); verify_resumed();
    memcpy(&saved,game+0x5800,8); CHECK(saved==123456); /* timed-out queue cleared */

    prepare_paused(1,21);
    CHECK(!pthread_create(&worker,NULL,paused_worker,NULL));
    CHECK(!dm_save_wait(1,&commits,&froze,&ms)); CHECK(!pthread_join(worker,NULL));
    CHECK(!commits && froze && ms<500); verify_resumed();

    prepare_paused(2,22);
    CHECK(!pthread_create(&worker,NULL,paused_worker,NULL));
    CHECK(dm_save_wait(.05,&commits,&froze,&ms)); CHECK(!pthread_join(worker,NULL));
    CHECK(!commits && froze && image.base[0x2d5ff51]==7 && image.base[0x2d5ff67]==9);
    CHECK(!dm_ingame_now()); image.base[0x2d5ff51]=image.base[0x2d5ff67]=0;
    tick(game); freeze_contract(0); CHECK(dm_ingame_now());

    prepare_paused(3,23);
    CHECK(!pthread_create(&worker,NULL,paused_worker,NULL));
    CHECK(!dm_save_wait(1,&commits,&froze,&ms)); CHECK(!pthread_join(worker,NULL));
    CHECK(!commits && froze && ms<500); verify_resumed();
    void (*unmount)(const char *)=binding("_ZN2nn2fs7UnmountEPKc"); unmount("adapter_fixture");
    FILE *out=fopen(argv[2],"w"); CHECK(out);
    fprintf(out,"{\"passed\":true,\"fixture_only\":true,\"mode\":\"d2r-save-hud-adapter\",\"checks\":%u,\"build_gate_checked\":true,\"checked_memory_access\":true,\"hud_totals_checked\":true,\"client_cursor_checked\":true,\"players_readback_hook_checked\":true,\"server_queries_checked\":true,\"opt_in_panel_freeze_checked\":true,\"stale_run_rejected\":true,\"paused_flags_preserved\":true,\"all_four_freeze_gates_checked\":true,\"paused_commit_quiet_ms\":200,\"timeout_cancel_stale_run_write_failure_rollback_checked\":true,\"inflight_tick_after_pause_restore_checked\":true,\"real_hle_commit_observed\":true,\"original_game_save_executed\":false,\"game_save_data_used\":false,\"full_game_verified\":false}\n",checks);
    CHECK(!fclose(out));
    printf("D2R_SAVE_ADAPTER_FIXTURE passed checks=%u\n",checks);
    return 0;
}
