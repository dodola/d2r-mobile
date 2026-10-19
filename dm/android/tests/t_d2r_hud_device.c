/* Exercises the shipped library's copied HUD ABI and draw-receipt guards.
 * Synthetic module/widget memory exercises production cohort identity checks;
 * it cannot claim original-game execution or a Java Canvas drew pixels. */
#include "../../host/dm.h"
#include "../../host/hle.h"
#include "../d2r_hud_android.h"
#include "../input_backend.h"
#include "../touch_controls_android.h"
#include "../../host/dm_hud.h"
#include "../../host/dm_input.h"
#include "../../ios/Sources/dm_touch_native_art.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static int checks;
/* Test-only ELF interposition drives a completed layout rebuild without
 * calling guest code or changing the production counter/identity guards. */
static uint32_t fixture_rebuilds;
uint32_t dm_padmode_rebuilds(void) {return fixture_rebuilds;}
static uint64_t message_hash(const char *text) {
    uint64_t hash=0xcbf29ce484222325ull;
    while(*text)hash=(hash^(uint8_t)*text++)*0x100000001b3ull;
    return hash;
}
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"HUD fixture failed line=%d\n",__LINE__);return 1;}}while(0)
static int ready(void *opaque) {(void)opaque;return 1;}
int main(int argc,char **argv) {
    assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib){fprintf(stderr,"dlopen %s\n",dlerror());return 1;}
    void (*publish)(const dm_hud_info *)=dlsym(lib,"dm_hud_publish");
    void (*resize)(float,float,float,float)=dlsym(lib,"dm_android_touch_resize");
    void (*enable)(int)=dlsym(lib,"dm_android_touch_set_enabled");
    void (*set_state)(const dm_android_touch_game_state *)=dlsym(lib,"dm_android_touch_set_game_state");
    void (*frame)(int64_t *)=dlsym(lib,"dm_android_hud_frame");
    void (*rendered)(const int64_t *,size_t,uint32_t)=dlsym(lib,"dm_android_hud_rendered");
    int (*allows)(const dm_tg_snapshot *,unsigned,uint64_t)=dlsym(lib,"dm_android_hud_receipt_allows");
    int (*hide)(uint64_t)=dlsym(lib,"dm_android_hud_hide_glyph");
    void (*message)(const char *,const char *)=dlsym(lib,"dm_android_hud_message");
    uint64_t (*clock_now)(void)=dlsym(lib,"now_ns");
    dm_ta_glyph (*current)(const dm_tg_snapshot *,uint64_t,uint64_t,int)=dlsym(lib,"dm_ta_current_set");
    int (*at)(int,int,dm_ta_frame *)=dlsym(lib,"dm_ta_frame_at");
    _Atomic int *language=dlsym(lib,"g_dm_lang");
    assert(publish&&resize&&enable&&set_state&&frame&&rendered&&allows&&hide&&message&&clock_now&&language);
    int64_t words[DM_AHUD_WORDS];dm_hud_info h={.valid=1,.level=42,.exp_permille=517,.belt={1,2,3,4},.hp=377,.hp_max=501,.mp=99,.mp_max=200,.players=8,.points=5,.quest_unread=1};
    dm_android_touch_game_state state={.pointer_item=-1,.belt_rows=4};
    resize(1280,720,0,0);enable(1);set_state(&state);publish(&h);frame(words);
    CHECK(words[0]==DM_AHUD_VERSION&&words[1]&&words[2]&&words[27]);
    CHECK(words[3]==42&&words[4]==517&&words[5]==377&&words[6]==501&&words[7]==99&&words[8]==200);
    CHECK(words[9]==1&&words[10]==2&&words[11]==3&&words[12]==4&&words[13]==8&&words[14]==5&&words[15]==1);
    dm_android_touch_snapshot touch;
    void (*touch_snapshot)(dm_android_touch_snapshot *)=dlsym(lib,"dm_android_touch_snapshot_get");
    assert(touch_snapshot);touch_snapshot(&touch);
    CHECK(words[DM_AHUD_TARGETS]==touch.controls.enabled_targets);
    CHECK(words[DM_AHUD_NAVIGATION]==touch.controls.navigation_mode);
    CHECK(!words[DM_AHUD_DIFF_OPEN]&&!words[DM_AHUD_MAP_CANCEL]&&!words[DM_AHUD_SWAP_HELD]);
    for(int i=0;i<4;i++){int lang[]={0,8,9,1};atomic_store(language,lang[i]);frame(words);CHECK(words[18]==(i==1||i==2));}
    h.dead=1;publish(&h);frame(words);CHECK(!words[1]&&words[16]);h.dead=0;
    h.valid=0;publish(&h);frame(words);CHECK(!words[1]&&!words[2]);h.valid=1;publish(&h);
    state.navigation=1;set_state(&state);frame(words);CHECK(!words[1]&&words[DM_AHUD_NAVIGATION]);
    message("OpenPanel","NpcDialogPanel");frame(words);CHECK(words[1]);
    message("OpenPanel","BankExpansionLayout");frame(words);CHECK(!words[1]);
    message("ClosePanel","BankExpansionLayout");message("ClosePanel","NpcDialogPanel");
    state.navigation=0;set_state(&state);frame(words);CHECK(words[1]);
    enable(0);frame(words);CHECK(!words[1]&&!words[27]);enable(1);
    CHECK(!hide(0)&&!hide(UINT64_MAX));

    if(!words[26]) {
        CHECK(!allows(NULL,0,clock_now()));
        printf("ANDROID_D2R_HUD_OK checks=%d atlas=0 numeric_snapshot=1 same_game_language=1 no_replacement_no_hide=1\n",checks);
        return 0;
    }
    assert(current&&at);
    dm_ta_glyph (*skill)(uint32_t)=dlsym(lib,"dm_ta_skill");assert(skill);
    dm_ta_glyph portal=skill(359);dm_ta_frame portal_frame;
    CHECK(portal.status==DM_TA_GLYPH&&at(portal.atlas,portal.normal_frame,&portal_frame));
    CHECK(words[DM_AHUD_PORTAL]==portal.status&&words[DM_AHUD_PORTAL+1]==portal.atlas&&words[DM_AHUD_PORTAL+8]==359);
    CHECK(words[DM_AHUD_PORTAL+2]==portal_frame.x&&words[DM_AHUD_PORTAL+3]==portal_frame.y&&words[DM_AHUD_PORTAL+4]==portal_frame.width);
    uint64_t now=clock_now();dm_tg_snapshot n={.captured_ns=now,.scene_epoch=11,.world_epoch=22,.binding_identity=33,.hud_identity=44,
        .ready=1,.supported=1,.valid=1,.source=DM_TG_SOURCE_NATIVE_GETTER,.identity_scope=DM_TG_IDENTITY_NATIVE};
    const uint32_t logical[6]={0x2000,0x1000,0x8000,0x4000,0x200,0x100};
    const uint64_t buttons[5]={DM_BTN_A,DM_BTN_B,DM_BTN_X,DM_BTN_Y,DM_BTN_R};
    for(int i=0;i<6;i++)n.mapping[i]=(dm_tg_mapping){logical[i],i,i+6};
    for(int i=0;i<12;i++)n.raw_skill_id[i]=0;
    memset(words,0,sizeof words);words[0]=DM_AHUD_VERSION;words[1]=words[2]=words[26]=words[27]=1;
    words[20]=now;words[21]=n.scene_epoch;words[22]=n.world_epoch;words[23]=n.binding_identity;words[24]=n.hud_identity;
    for(int i=0;i<5;i++) {
        dm_ta_glyph g=current(&n,buttons[i],now,0);dm_ta_frame normal,pressed;
        CHECK(g.status==DM_TA_GLYPH&&g.slot==i&&at(g.atlas,g.normal_frame,&normal)&&at(g.atlas,g.pressed_frame,&pressed));
        int64_t *v=words+DM_AHUD_HEAD+i*DM_AHUD_GLYPH;
        v[0]=g.status;v[1]=g.atlas;v[2]=normal.x;v[3]=normal.y;v[4]=normal.width;v[5]=normal.height;
        v[6]=pressed.x;v[7]=pressed.y;v[8]=g.raw_skill_id;v[9]=g.slot;v[10]=g.effective_slot;
        CHECK(normal.width==65&&normal.height==65&&pressed.x==normal.x+68);
    }
    rendered(NULL,0,0);CHECK(!allows(&n,0,clock_now()));
    rendered(words,DM_AHUD_WORDS,0);CHECK(!allows(&n,0,clock_now()));
    rendered(words,DM_AHUD_WORDS,31);
    for(unsigned i=0;i<5;i++)CHECK(allows(&n,i,clock_now()));
    CHECK(!allows(&n,5,clock_now())&&!allows(&n,6,clock_now()));
    for(unsigned i=0;i<5;i++) {
        rendered(words,DM_AHUD_WORDS,1u<<i);
        for(unsigned j=0;j<5;j++)CHECK(allows(&n,j,clock_now())==(i==j));
    }
    rendered(words,DM_AHUD_WORDS,31);
    const int fields[]={0,1,2,17,20,21,22,23,24,25,26,27,DM_AHUD_HEAD,DM_AHUD_HEAD+1,DM_AHUD_HEAD+8,DM_AHUD_HEAD+9,DM_AHUD_HEAD+10};
    for(unsigned i=0;i<sizeof fields/sizeof *fields;i++) {
        int k=fields[i];int64_t save=words[k];words[k]=k==20?now+1:save+1;
        /* Boolean flags are invalidated with zero rather than another true value. */
        if(k==1||k==2||k==26||k==27)words[k]=0;
        rendered(words,DM_AHUD_WORDS,1);CHECK(!allows(&n,0,clock_now()));words[k]=save;
    }
    words[20]=now-DM_TG_MAX_AGE_NS-1;rendered(words,DM_AHUD_WORDS,31);CHECK(!allows(&n,0,clock_now()));words[20]=now;
    rendered(words,DM_AHUD_WORDS-1,31);CHECK(!allows(&n,0,clock_now()));
    rendered(words,DM_AHUD_WORDS,31);CHECK(!allows(&n,0,clock_now()+DM_TG_MAX_AGE_NS+1));
    CHECK(!allows(&n,0,now-1));
    dm_tg_snapshot old=n;old.captured_ns=now-DM_TG_MAX_AGE_NS-1;CHECK(!allows(&old,0,clock_now()));
    old=n;old.ready=0;CHECK(!allows(&old,0,clock_now()));old=n;old.valid=0;CHECK(!allows(&old,0,clock_now()));
    old=n;old.supported=0;CHECK(!allows(&old,0,clock_now()));old=n;old.source=0;CHECK(!allows(&old,0,clock_now()));
    old=n;old.identity_scope=DM_TG_IDENTITY_SYNTHETIC;CHECK(!allows(&old,0,clock_now()));
    old=n;old.selected_bank=16;CHECK(!allows(&old,0,clock_now()));
    old=n;old.mapping[1]=old.mapping[0];CHECK(!allows(&old,0,clock_now()));
    old=n;old.raw_skill_id[0]=UINT32_MAX;CHECK(!allows(&old,0,clock_now()));
    rendered(NULL,0,0);CHECK(!allows(&n,0,clock_now())&&!hide(0));

    /* Build the original getter's saved-frame/widget shape and feed the
     * production observation gate, without SDK_STUB or identity overrides. */
    module_t *mods=dlsym(lib,"g_mod");int *nmods=dlsym(lib,"g_nmod");
    void (*gate)(uint64_t,const char *,int,uint64_t)=dlsym(lib,"gate_register");
    uint64_t (*dispatch)(uint64_t,uint64_t *)=dlsym(lib,"gate_dispatch");
    int (*host_attach)(void)=dlsym(lib,"dm_android_d2r_host_attach");
    int (*bridge_attach)(void)=dlsym(lib,"dm_touch_game_attach");
    void (*set_scene)(int,uint64_t,uint64_t)=dlsym(lib,"dm_touch_game_set_scene");
    int (*snapshot)(dm_tg_snapshot *)=dlsym(lib,"dm_touch_game_snapshot");
    int (*host_call)(int,uint64_t *)=dlsym(lib,"dm_android_title_host_call");
    int (*bind)(const dm_android_input_config *)=dlsym(lib,"dm_android_input_bind");
    int (*controller)(uint64_t,int32_t,int32_t,int32_t,int32_t)=dlsym(lib,"dm_android_input_publish_controller_actions");
    void (*touched)(void)=dlsym(lib,"dm_android_input_touch_activity");
    assert(mods&&nmods&&gate&&dispatch&&host_attach&&bridge_attach&&set_scene&&snapshot&&host_call&&bind&&controller&&touched);
    *nmods=1;strcpy(mods[0].name,"main");strcpy(mods[0].build_id,"db0507f36b84200ac05f75d76ed5026f");
    mods[0].size=0x2f10000;mods[0].base=calloc(1,mods[0].size);assert(mods[0].base);
    gate(0x6000,"obs",6,0);gate(0x7000,"obs",OBS_UIMSG,0);gate(0x8500,"svc",SVC_HOST_PRIVATE_SKILL_RENDER,0);
    CHECK(host_attach()&&bridge_attach());set_scene(1,100,200);
    dm_tg_mapping table[6];for(int i=0;i<6;i++)table[i]=(dm_tg_mapping){i==5?0x800:logical[i],i,i+6};
    memcpy(mods[0].base+0x2d13cbc,table,sizeof table);
    unsigned char widgets[7][0xc91];memset(widgets,0,sizeof widgets);uint64_t base=(uintptr_t)mods[0].base,vt=base+0x208a5f8;
    for(unsigned i=0;i<7;i++){uint32_t slot=i<6?i:0;memcpy(widgets[i],&vt,8);memcpy(widgets[i]+0xb18,&slot,4);}
    uint64_t regs[31]={0},saved_frame[3]={0};saved_frame[2]=base+0x3e2f74;
    regs[10]=base+0x2d5f0c0;regs[29]=(uintptr_t)&saved_frame[1];
    for(unsigned pass=0;pass<2;pass++)for(unsigned slot=0;slot<6;slot++) {
        regs[8]=slot*DM_TG_STRIDE;regs[19]=regs[23]=slot;saved_frame[0]=(uintptr_t)widgets[slot];dispatch(0x6000,regs);
    }
    CHECK(snapshot(&n)&&n.ready&&n.group_seen==63&&n.identity_scope==DM_TG_IDENTITY_NATIVE);
    CHECK(n.scene_epoch==100&&n.world_epoch==200&&n.hud_identity&&n.binding_identity);
    frame(words);CHECK(words[1]&&words[21]==100&&words[22]==200&&words[24]==(int64_t)n.hud_identity);
    for(int i=0;i<5;i++)CHECK(words[DM_AHUD_HEAD+i*DM_AHUD_GLYPH]==DM_TA_GLYPH);
    rendered(NULL,0,0);CHECK(!hide((uintptr_t)widgets[0]));rendered(words,DM_AHUD_WORDS,31);
    for(int slot=0;slot<5;slot++) {
        for(int i=0;i<31;i++)regs[i]=0x12340000u+i;regs[0]=(uintptr_t)widgets[slot];uint64_t before[31];memcpy(before,regs,sizeof before);
        CHECK(host_call(SVC_HOST_PRIVATE_SKILL_RENDER,regs)&&regs[8]==1);
        for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==before[i]);
    }
    CHECK(!hide((uintptr_t)widgets[5])&&!hide((uintptr_t)widgets[6]));
    widgets[0][0xc90]=1;CHECK(!hide((uintptr_t)widgets[0]));widgets[0][0xc90]=0;
    widgets[0][0]^=1;CHECK(!hide((uintptr_t)widgets[0]));widgets[0][0]^=1;
    CHECK(hide((uintptr_t)widgets[0]));
    dm_android_input_config config={.source="isolated-hud-fixture",.source_ready=ready,.panel_width=1280,.panel_height=720,.logical_npad_style=1};
    CHECK(!bind(&config)&&!controller(DM_BTN_A,0,0,0,0));CHECK(!hide((uintptr_t)widgets[0]));
    touched();CHECK(hide((uintptr_t)widgets[0]));
    enable(0);CHECK(!hide((uintptr_t)widgets[0]));enable(1);CHECK(hide((uintptr_t)widgets[0]));
    rendered(words,DM_AHUD_WORDS,0);CHECK(!hide((uintptr_t)widgets[0]));
    for(int i=0;i<31;i++)regs[i]=0x56780000u+i;regs[0]=(uintptr_t)widgets[0];uint64_t before[31];memcpy(before,regs,sizeof before);
    CHECK(host_call(SVC_HOST_PRIVATE_SKILL_RENDER,regs)&&!regs[8]);for(int i=0;i<31;i++)if(i!=8)CHECK(regs[i]==before[i]);
    rendered(words,DM_AHUD_WORDS,31);set_scene(1,101,201);CHECK(!hide((uintptr_t)widgets[0]));
    /* Real Android producer consumes the same completed-rebuild counter as
     * front_ios. New widgets must relearn, and an old draw receipt must never
     * authorize hiding either the retired set or its replacement. */
    void (*touch_tick)(void)=dlsym(lib,"dm_android_touch_game_tick");assert(touch_tick);
    char panel[]="LoadScreenPanel";
    uint64_t message_fields[]={message_hash("PanelManager"),message_hash("UnloadPanel"),(uintptr_t)panel};
    memset(regs,0,sizeof regs);regs[1]=(uintptr_t)message_fields;dispatch(0x7000,regs);touch_tick();
    CHECK(!snapshot(&n)&&n.world_epoch==2);
    regs[10]=base+0x2d5f0c0;regs[29]=(uintptr_t)&saved_frame[1];
    for(unsigned pass=0;pass<2;pass++)for(unsigned slot=0;slot<6;slot++) {
        regs[8]=slot*DM_TG_STRIDE;regs[19]=regs[23]=slot;saved_frame[0]=(uintptr_t)widgets[slot];dispatch(0x6000,regs);
    }
    CHECK(snapshot(&n)&&n.ready&&n.world_epoch==2);uint64_t old_hud=n.hud_identity;
    frame(words);rendered(words,DM_AHUD_WORDS,31);CHECK(hide((uintptr_t)widgets[0]));
    int64_t retired[DM_AHUD_WORDS];memcpy(retired,words,sizeof retired);
    unsigned char replacement[6][0xc91];memset(replacement,0,sizeof replacement);
    for(unsigned i=0;i<6;i++){memcpy(replacement[i],&vt,8);memcpy(replacement[i]+0xb18,&i,4);}
    fixture_rebuilds=1;touch_tick();CHECK(!snapshot(&n)&&!n.ready&&n.world_epoch==3);
    CHECK(!hide((uintptr_t)widgets[0]));
    for(unsigned pass=0;pass<2;pass++) {
        for(unsigned slot=0;slot<6;slot++) {
            regs[8]=slot*DM_TG_STRIDE;regs[19]=regs[23]=slot;saved_frame[0]=(uintptr_t)replacement[slot];dispatch(0x6000,regs);
        }
        if(!pass)CHECK(!snapshot(&n)&&!n.ready);
    }
    CHECK(snapshot(&n)&&n.ready&&n.group_seen==63&&n.world_epoch==3&&n.hud_identity!=old_hud);
    rendered(retired,DM_AHUD_WORDS,31);CHECK(!hide((uintptr_t)replacement[0]));
    frame(words);for(unsigned i=0;i<5;i++)CHECK(words[DM_AHUD_HEAD+i*DM_AHUD_GLYPH]==DM_TA_GLYPH);
    rendered(words,DM_AHUD_WORDS,31);CHECK(hide((uintptr_t)replacement[0])&&!hide((uintptr_t)widgets[0]));
    uint64_t replacement_hud=n.hud_identity;touch_tick();
    CHECK(snapshot(&n)&&n.ready&&n.world_epoch==3&&n.hud_identity==replacement_hud);
    printf("ANDROID_D2R_HUD_OK checks=%d atlas=9 numeric_snapshot=1 same_game_language=1 visibility=1 five_skill_mapping=1 draw_receipt_guards=1 production_identity_gate=1 render_x8_only=1 synthetic_memory=1 java_canvas_not_claimed=1 original_game_not_claimed=1\n",checks);
    return 0;
}
