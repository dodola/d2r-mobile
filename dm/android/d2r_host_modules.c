#include "d2r_host_modules.h"
#include "d2r_save_adapter.h"
#include "d2r_debug_mode.h"
#include "../host/dm.h"
#include "../host/hle.h"
#include "../host/dm_hud.h"
#include "touch_controls_android.h"
#include "../ios/Sources/dm_touch_controls.h"
#include "../ios/Sources/dm_touch_game_identity.h"
#include "../host/dm_rift.h"
#include "../host/dm_bossrush.h"
#include <stdatomic.h>
#ifdef DM_ANDROID_D2R_UI
#include "d2r_ui_adapter.h"
#include "d2r_hud_android.h"
#include "d2r_world_overlay_android.h"
#endif

/* The adapter is linked only by the D2R Activity target.  Artifact gates are
 * still required: older/portable builds simply omit each private feature.
 * No startup callback reads a guest heap object before guest initialization. */
static _Atomic int g_attached;
static module_t *g_d2r_main;
static dm_tc_scene g_scene;
static int g_navigation = 1;
static int g_panel_bits;
static pthread_mutex_t g_scene_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_scene_epoch = 1, g_world_epoch = 1;
static uint32_t g_padmode_rebuilds;
static uint64_t g_uimsg_count;
static int g_private_gameplay;
#ifdef DM_ANDROID_D2R_UI
static int g_ui_enabled;
#endif
static _Atomic int g_quiet_diff;
enum { MESSAGE_NAME_BITS=18, MESSAGE_NAME_COUNT=1u<<MESSAGE_NAME_BITS };
static struct { uint64_t hash; const char *text; } *g_message_names;

/* Kept in the verified title adapter rather than the generic Android SDK
 * backend. The upstream Rift module toggles this original packet filter. */
void dm_quiet_difficulty_msg(int on) { atomic_store(&g_quiet_diff,on); }
static void d2r_event_message(const uint64_t *regs) {
    uint8_t event[2];
    if (!regs || !atomic_load(&g_quiet_diff) || !dm_tg_vm_read(regs[0],event,sizeof event) || event[1]!=0x17) return;
    ((uint8_t *)(uintptr_t)regs[0])[1]=0xff;
}
static void d2r_c2s(const uint64_t *regs) {
    uint8_t packet[512];
    if (!regs) return;
    uint32_t length=(uint32_t)regs[3];if(length>sizeof packet)length=sizeof packet;
    if (!length || !dm_tg_vm_read(regs[2],packet,length)) return;
    dm_bossrush_c2s(regs[0],regs[1],packet,length);
    dm_rift_c2s(regs[0],packet,length);
#ifdef DM_ANDROID_D2R_UI
    if(g_ui_enabled) {
        void dm_pouch_c2s(uint64_t *);
        dm_pouch_c2s((uint64_t *)(uintptr_t)regs);
        dm_android_d2r_ui_c2s(regs[0],regs[1],packet,length);
    }
#endif
}

#ifdef DM_ANDROID_D2R_UI
/* Product packet rules from diag.c. These run in the original receive pump,
 * before 0x632510 dispatches a command, so belt/inventory decisions inspect
 * server state at handling time rather than the client's earlier snapshot. */
typedef struct { uint64_t a;uint32_t b,c;uint8_t ok,pad[47]; } d2r_autoplace_result;
static int d2r_pickup(const uint64_t *regs,uint8_t packet[21],uint32_t *id) {
    if(regs[1]!=17 || !dm_tg_vm_read(regs[0],packet,21) || packet[4]!=0x16)return 0;
    memcpy(id,packet+5,4);return 1;
}
static uint64_t d2r_ground_item(uint32_t id) {
    uint64_t item=dm_diag_srv_item(id);uint32_t mode=0;
    return item && dm_tg_vm_read(item+0xc,&mode,4) && mode==3?item:0;
}
static int d2r_potpick_gate(const uint64_t *regs) {
    int dm_potpick_auto(uint32_t);
    uint8_t packet[21];uint32_t id=0,slot=0;
    if(!d2r_pickup(regs,packet,&id) || !dm_potpick_auto(id))return 0;
    uint64_t game=0,player=0,inv=0,item=0;
    if(!dm_android_d2r_server_context(&game,&player) ||
       !(item=d2r_ground_item(id)) || !dm_tg_vm_read(player+0x90,&inv,8) || !inv)return 0;
    int fits=((int (*)(uint64_t,uint64_t,uint32_t *,int))(g_d2r_main->base+0x561de0))(inv,item,&slot,1);
    static unsigned logged;
    if(!fits && logged++<40)LOG("ANDROID_D2R_POTPICK belt_full=1 item=%u left_on_ground=1",id);
    return !fits;
}
static int d2r_cube_pickup(const uint64_t *regs,uint8_t out[29]) {
    static int on=-1;
    if(on<0) { const char *option=getenv("DM_CUBE_PICKUP");on=!(option && !strcmp(option,"0")); }
    uint8_t packet[21];uint32_t id=0,flag=0;
    if(!on || !d2r_pickup(regs,packet,&id))return 0;
    memcpy(&flag,packet+17,4);if(!flag)return 0;
    uint64_t game=0,player=0,item=0,player_game=0,inv=0,path=0;
    if(!dm_android_d2r_server_context(&game,&player) || !(item=d2r_ground_item(id)))return 0;
    uint8_t *base=g_d2r_main->base;
    int (*is_type)(uint64_t,int)=(void *)(base+0x574bc0);
    if(is_type(item,0x16) || is_type(item,0x12) || ((int (*)(uint64_t))(base+0x5722f0))(item))return 0;
    if(!dm_tg_vm_read(player+0xd8,&player_game,8) || !player_game ||
       !dm_tg_vm_read(player+0x90,&inv,8) || !inv)return 0;
    if(((d2r_autoplace_result (*)(uint64_t,uint64_t,uint64_t))(base+0x646840))(player_game,player,item).ok)return 0;
    uint64_t cube=((uint64_t (*)(uint64_t))(base+0x565e50))(player);if(!cube)return 0;
    uint32_t tx=0,ty=0,cid=0,cx=0,cy=0;
    if(!((int (*)(uint64_t,uint64_t,int,uint32_t *,uint32_t *,int))(base+0x55f620))(inv,item,9,&tx,&ty,3))return 0;
    if(!dm_tg_vm_read(cube+8,&cid,4) || !dm_tg_vm_read(cube+0x38,&path,8) || !path ||
       !dm_tg_vm_read(path+0x10,&cx,4) || !dm_tg_vm_read(path+0x14,&cy,4))return 0;
    uint32_t cxy=(cx&0xffff)|cy<<16,txy=(tx&0xffff)|ty<<16;
    memcpy(out,packet,4);out[4]=0x5f;memcpy(out+5,&id,4);memcpy(out+9,&cid,4);
    memcpy(out+13,packet+9,8);memcpy(out+21,&cxy,4);memcpy(out+25,&txy,4);
    static unsigned logged;
    if(logged++<40)LOG("ANDROID_D2R_CUBE_PICKUP item=%u cube=%u slot=%u,%u",id,cid,tx,ty);
    return 1;
}
static int d2r_drop_ground_move(const uint64_t *regs) {
    extern _Atomic uint64_t g_dm_move_allow_until_ns;
    uint64_t dm_stick_last_ns(void);
    uint8_t command=0xff;
    if(!regs[0] || regs[0]>UINT64_MAX-5 || regs[1]<1 ||
       !dm_tg_vm_read(regs[0]+4,&command,1) || (command!=1 && command!=3))return 0;
    uint64_t now=now_ns();
    return now-dm_stick_last_ns()>700000000ull && now>=atomic_load(&g_dm_move_allow_until_ns);
}
#endif
int dm_android_title_ui_override_allowed(void) { return g_attached; }

/* Only D2R's reviewed Terror Zone callers may use the Boss Rush clock. */
int dm_android_title_clock_override(uint64_t caller,int64_t *out) {
    extern _Atomic int64_t g_dm_tz_time;
    extern _Atomic uint64_t g_dm_tz_live_ns;
    int64_t value=atomic_load(&g_dm_tz_time);
    if (!g_attached || !g_private_gameplay || !value || !out || now_ns()>=atomic_load(&g_dm_tz_live_ns)) return 0;
    uint64_t base=(uintptr_t)g_d2r_main->base;
    if(caller<base || caller-base>=0x1b786a0) return 0;
    uint64_t offset=caller-base;
    int match=offset>0x58f1e0 && offset<0x591440;
    uint32_t instructions[8];
    if(!match && dm_tg_vm_read(caller,instructions,sizeof instructions))for(int k=0;k<8;k++) {
        uint32_t instruction=instructions[k];
        if((instruction&0xfc000000)!=0x94000000)continue;
        int64_t immediate=instruction&0x3ffffff;if(immediate&0x2000000)immediate-=1<<26;
        if((int64_t)offset+4*k+immediate*4==0x58f310){match=1;break;}
    }
    if(match)*out=value;
    return match;
}
static void d2r_players_apply(void) {
    static int applied;
    int want=dm_hud_requested_players();uint32_t mode=0;
    if(!want || want==applied)return;
    applied=want;
    if(!dm_tg_vm_read((uintptr_t)g_d2r_main->base+0x2d20b8c,&mode,4) || (mode!=0 && mode!=5 && mode!=7))return;
    void *object=((void *(*)(void))(g_d2r_main->base+0x3102c0))();
    if(object)((void (*)(void *,uint32_t))(g_d2r_main->base+0x1199c50))(object,(uint32_t)want);
}
int dm_android_d2r_players_get(uint8_t *main_base) {
    if(!g_attached || main_base!=g_d2r_main->base || !gate_obs_present(OBS_TICK) || g_d2r_main->size<0x3102c4)return 0;
    void *object=((void *(*)(void))(main_base+0x3102c0))();uint32_t players=0;
    if(!object || !dm_tg_vm_read((uintptr_t)object+0xc,&players,4) || players<1 || players>8)return 0;
    return (int)players;
}
/* The save adapter calls this after releasing its own mutex. Private game
 * modules can request a save without recursively acquiring that mutex. */
void dm_android_d2r_rift_tick(uint64_t game,uint64_t player,uint8_t *main_base) {
    if(!g_attached || !g_private_gameplay || !game || !player || main_base!=g_d2r_main->base)return;
    d2r_players_apply();
    dm_bossrush_tick(game,player,main_base);
    dm_rift_tick(game,player,main_base);
}

extern void dm_android_uimsg(const char *, const char *, const char *);
extern void dm_touch_game_set_scene(int, uint64_t, uint64_t);

/* Same FNV-1a message contract as diag.c's upstream OBS_UIMSG decoder.  These
 * are the categories/actions consumed by the portable scene and settings
 * state machines; unknown hashes are preserved for other subscribers. */
static uint64_t message_hash(const char *s) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (; *s; ++s) hash = (hash ^ (uint8_t)*s) * 0x100000001b3ull;
    return hash;
}
/* Same reverse FNV table as upstream diag.c. Title modules consume many UI
 * messages beyond panel navigation (character creation, tabs, dropdowns).
 * The verified module's RO/data metadata bounds every scanned string. */
static void message_names_build(module_t *main) {
    if(!main->ro_off || main->ro_off>=main->data_off || main->data_off>main->size)return;
    g_message_names=calloc(MESSAGE_NAME_COUNT,sizeof *g_message_names);
    if(!g_message_names)return;
    const char *p=(const char *)main->base+main->ro_off,*end=(const char *)main->base+main->data_off;
    while(p<end) {
        const char *q=p;while(q<end && *q>=0x20 && *q<0x7f)q++;
        if(q<end && !*q && q-p>=3) {
            uint64_t hash=message_hash(p);unsigned slot=(unsigned)(hash>>(64-MESSAGE_NAME_BITS));
            for(unsigned probe=0;probe<MESSAGE_NAME_COUNT;probe++,slot=(slot+1)&(MESSAGE_NAME_COUNT-1)) {
                if(g_message_names[slot].text && g_message_names[slot].hash!=hash)continue;
                g_message_names[slot].hash=hash;g_message_names[slot].text=p;break;
            }
        }
        if(q==end)break;
        p=q+1;
    }
}
static const char *message_name(uint64_t hash, char *fallback, size_t n) {
    if(g_message_names) {
        unsigned slot=(unsigned)(hash>>(64-MESSAGE_NAME_BITS));
        for(unsigned probe=0;probe<MESSAGE_NAME_COUNT && g_message_names[slot].text;probe++,slot=(slot+1)&(MESSAGE_NAME_COUNT-1))
            if(g_message_names[slot].hash==hash)return g_message_names[slot].text;
    }
    static const char *const names[] = {
        "PanelManager", "FrontEndNavigation", "OptionsPanelMessage",
        "OpenPanel", "OpenExclusivePanel", "ClosePanel", "UnloadPanel", "UnloadAll", "TogglePanel",
        "ToGame", "ToFrontEnd", "AcceptChanges", "CancelChanges", "ResetToDefaults"
    };
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i)
        if (hash == message_hash(names[i])) return names[i];
    snprintf(fallback, n, "%#llx", (unsigned long long)hash);
    return fallback;
}
/* Portable message observations are a fallback until the title adapter can
 * publish a verified widget-tree snapshot. Keep an independent panel state:
 * dm_tc_scene_dead() reads the last published state, not the current message. */
static void panel_message(const char *category, const char *action, const char *argument) {
    static const struct { const char *name; int bit; } panels[] = {
        {"BankExpansionLayout",1}, {"NpcTradePanel",2}, {"YouDiedModal",4},
        {"TextInputModal",8}, {"ImbueItemsPanel",16}, {"HoradricCubeLayout",32}
    };
    if (!strcmp(category,"FrontEndNavigation") && !strcmp(action,"ToFrontEnd")) g_panel_bits=0;
    if (strcmp(category,"PanelManager")) return;
    if (!strcmp(action,"UnloadAll")) { g_panel_bits=0; return; }
    for (unsigned i=0; i<sizeof panels/sizeof *panels; ++i) if (!strcmp(argument,panels[i].name)) {
        if (!strcmp(action,"OpenPanel") || !strcmp(action,"OpenExclusivePanel")) g_panel_bits|=panels[i].bit;
        else if (!strcmp(action,"ClosePanel") || !strcmp(action,"UnloadPanel")) g_panel_bits&=~panels[i].bit;
        else if (!strcmp(action,"TogglePanel")) g_panel_bits^=panels[i].bit;
    }
}
static void d2r_uimsg(const uint64_t *regs) {
    uint64_t fields[3];
    if (!regs || !dm_tg_vm_read(regs[1], fields, sizeof fields)) return;
    char category_buf[24], action_buf[24], argument[65] = "";
    const char *category = message_name(fields[0], category_buf, sizeof category_buf);
    const char *action = message_name(fields[1], action_buf, sizeof action_buf);
    if (fields[2] && dm_tg_vm_read(fields[2], argument, 64)) {
        int terminated = 0;
        for (unsigned i = 0; i < 64; ++i) {
            unsigned char c = (unsigned char)argument[i];
            if (!c) { terminated = 1; break; }
            if (c < 0x20 || c >= 0x7f) break;
        }
        if (!terminated) argument[0] = '\0';
    } else argument[0] = '\0';
    pthread_mutex_lock(&g_scene_lock);
    ++g_uimsg_count;
    dm_tc_scene before = g_scene;
    panel_message(category,action,argument);
    int navigation = dm_tc_navigation(&g_scene, category, action, argument);
    if (before.game != g_scene.game) ++g_world_epoch;
    if (navigation != g_navigation || before.game != g_scene.game || before.panels != g_scene.panels) ++g_scene_epoch;
    g_navigation = navigation;
    pthread_mutex_unlock(&g_scene_lock);
    { void dm_textinput_on_uimsg(const char *, const char *, const char *);
      dm_textinput_on_uimsg(category, action, argument); }
    { void dm_custom_on_uimsg(const char *, const char *, const char *);
#ifdef DM_ANDROID_D2R_UI
      if(!g_ui_enabled)
#endif
      dm_custom_on_uimsg(category, action, argument); }
    { void dm_android_custom_message(const char *,const char *,const char *);
      dm_android_custom_message(category,action,argument); }
    dm_rift_ui_on_uimsg(category,action,argument);
    { void dm_padmode_on_uimsg(const char *,const char *,const char *);
      dm_padmode_on_uimsg(category,action,argument); }
#ifdef DM_ANDROID_D2R_UI
    dm_android_d2r_ui_message(category,action,argument);
#endif
    { extern int g_swkbd_name_panel;
      if ((!strcmp(category,"PanelManager") && !strcmp(action,"UnloadAll")) ||
          (!strcmp(category,"FrontEndNavigation") && !strcmp(action,"ToFrontEnd"))) g_swkbd_name_panel=0;
      else if (!strcmp(category,"PanelManager") && !strcmp(argument,"CharacterCreateNamePanel")) {
          if (!strcmp(action,"OpenPanel") || !strcmp(action,"OpenExclusivePanel")) g_swkbd_name_panel=1;
          else if (!strcmp(action,"ClosePanel") || !strcmp(action,"UnloadPanel")) g_swkbd_name_panel=0;
      } }
    dm_android_uimsg(category, action, argument);
}

void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *out) {
    if(!out)return;
    pthread_mutex_lock(&g_scene_lock);
    *out=(dm_android_d2r_scene_status){.attached=atomic_load(&g_attached),
        .game=g_scene.game,.navigation=g_navigation,.panels=g_scene.panels,
        .scene_epoch=g_scene_epoch,.world_epoch=g_world_epoch,.uimsg_count=g_uimsg_count};
    pthread_mutex_unlock(&g_scene_lock);
}

static int d2r_read_touch_state(void *opaque, dm_android_touch_game_state *state) {
    (void)opaque;
    if (!g_attached || !gate_obs_present(OBS_UIMSG)) return 0;
    /* The original text-input completion path must run from the guest input
     * poll, after its opening UI message has unwound (same as iOS). */
    { void dm_textinput_pump(void); dm_textinput_pump(); }
    /* Match upstream's HID-poll pump: a released aiming stick restores the
     * temporary target/skill-record changes even when no further cast occurs. */
    if (gate_svc_present(SVC_HOST_PRIVATE_SKILL_AIM)) {
        void dm_aim_cone_pump(void); dm_aim_cone_pump();
    }
    dm_android_touch_get_game_state(state);
    pthread_mutex_lock(&g_scene_lock);
    /* Match front_ios: a completed layout rebuild replaces all six HUD
     * widgets. Retire the old cohort before learning the replacement set;
     * a scene-only reset would retain its stale widget addresses forever. */
    { uint32_t dm_padmode_rebuilds(void); uint32_t rebuilds=dm_padmode_rebuilds();
      if(rebuilds!=g_padmode_rebuilds) {g_padmode_rebuilds=rebuilds;++g_world_epoch;} }
    state->navigation = g_navigation;
    state->panel_bits = g_panel_bits;
    state->target_mask = g_navigation ? dm_tc_nav_mask(!(g_panel_bits & 4)) : 0;
    dm_touch_game_set_scene(!g_navigation, g_scene_epoch, g_world_epoch);
    pthread_mutex_unlock(&g_scene_lock);
    state->holding = dm_hud_held() != 0;
    if (state->belt_rows < 1) state->belt_rows = 4;
#ifdef DM_ANDROID_D2R_UI
    dm_android_d2r_ui_pump(state);
    dm_android_world_overlay_tick(state);
#endif
    return 1;
}

int dm_android_d2r_host_attach(void) {
    if (g_attached) return 1;
    module_t *main = NULL;
    for (int i = 0; i < g_nmod; ++i)
        if (!strcmp(g_mod[i].name, "main")) { main = &g_mod[i]; break; }
    /* Reviewed private layouts belong only to this exact D2R build. The
     * loader preserves the rewriter's BUILD_ID in module metadata; legacy
     * artifacts without that provenance remain usable by generic HLE only.
     * Image size is an additional bounds guard, never a version identity. */
    if (!main || !main->base || strcmp(main->build_id, "db0507f36b84200ac05f75d76ed5026f") || main->size < 0x2f05460) {
        LOG("ANDROID_D2R_HOST_ATTACH disabled build_id=%s expected=db0507f36b84200ac05f75d76ed5026f",
            main && main->build_id[0] ? main->build_id : "unknown");
        return 0;
    }
    { void dm_textbox_install(uint8_t *); dm_textbox_install(main->base); }
    { void dm_turnsmooth_install(uint8_t *); dm_turnsmooth_install(main->base); }
    dm_android_d2r_save_attach(main);
#ifdef DM_ANDROID_DEBUG_CONSOLE
    dm_android_d2r_debug_attach(main);
#endif
    g_d2r_main = main;
    message_names_build(main);
    g_attached = 1;
    { void dm_padmode_boot(void); dm_padmode_boot(); }
#ifdef DM_ANDROID_D2R_UI
    g_ui_enabled=dm_android_d2r_ui_attach(main);
#endif
    g_private_gameplay=main->size>=0x3211700 && gate_obs_present(OBS_TICK) && gate_obs_present(OBS_C2S) &&
        gate_obs_present(OBS_UIMSG) && gate_svc_present(SVC_HOST_RIFT_EXIT) && gate_svc_present(SVC_HOST_SAVE_ACT);
    if(g_private_gameplay) {
        gate_obs_set(OBS_C2S,d2r_c2s);
        if(gate_obs_present(OBS_EVENT_MSG))gate_obs_set(OBS_EVENT_MSG,d2r_event_message);
    }
#ifdef DM_ANDROID_D2R_UI
    else if(g_ui_enabled && gate_obs_present(OBS_C2S))gate_obs_set(OBS_C2S,d2r_c2s);
#endif
    if (gate_obs_present(OBS_UIMSG)) {
        gate_obs_set(OBS_UIMSG, d2r_uimsg);
#ifdef DM_ANDROID_D2R_UI
        dm_android_touch_bind_game_bridge(d2r_read_touch_state, g_ui_enabled ? dm_android_d2r_ui_events : NULL, NULL);
#else
        dm_android_touch_bind_game_bridge(d2r_read_touch_state, NULL, NULL);
#endif
    }
    LOG("ANDROID_D2R_HOST_ATTACH textbox=%d turn_smooth=%d model_yaw=%d drop_light=%d drop_sound=%d join_spawn=%d join_corpse=%d",
        gate_obs_present(OBS_TEXTBOX), gate_obs_present(OBS_ERR_SMOOTH),
        gate_obs_present(OBS_MODEL_YAW), gate_svc_present(SVC_HOST_GROUND_ASSET),
        gate_svc_present(SVC_HOST_DROP_SOUND), gate_svc_present(SVC_HOST_JOIN_SPAWN),
        gate_svc_present(SVC_HOST_JOIN_CORPSE));
    LOG("ANDROID_D2R_UI_ATTACH uimsg=%d textinput=%d native_scene=%d",
        gate_obs_present(OBS_UIMSG), gate_obs_present(OBS_UIMSG), gate_obs_present(OBS_UIMSG));
    LOG("ANDROID_D2R_PRIVATE_ATTACH skill_aim=%d skill_render=%d glyph_hide_requires_draw_receipt=1 rift_bossrush=%d",
        gate_svc_present(SVC_HOST_PRIVATE_SKILL_AIM),gate_svc_present(SVC_HOST_PRIVATE_SKILL_RENDER),g_private_gameplay);
    return 1;
}

int dm_android_title_host_call(int number, uint64_t *registers) {
    if (!g_attached || !registers || !gate_svc_present(number)) return 0;
    switch (number) {
    case SVC_HOST_TACT_ZLIB: {
        int dm_android_tact_call(int, uint64_t *);
        return dm_android_tact_call(number, registers);
    }
    case SVC_HOST_C2S_DELIVER: {
        void (*deliver)(uint64_t, uint64_t, uint64_t, uint64_t) =
            (void *)(g_d2r_main->base + 0x632510);
#ifdef DM_ANDROID_D2R_UI
        if(g_ui_enabled) {
            if(d2r_drop_ground_move(registers) || d2r_potpick_gate(registers))return 1;
            uint8_t cube[29];
            if(d2r_cube_pickup(registers,cube)) {
                deliver((uintptr_t)cube,25,registers[2],registers[3]);return 1;
            }
        }
#endif
        deliver(registers[0], registers[1], registers[2], registers[3]); return 1;
    }
    case SVC_HOST_C2S_SYS: {
        void (*deliver)(uint64_t, uint64_t) = (void *)(g_d2r_main->base + 0x6310b0);
        deliver(registers[0], registers[1]); return 1;
    }
    case SVC_HOST_GRANNY_A:
    case SVC_HOST_GRANNY_B: {
        int dm_android_granny_call(int, uint64_t *);
        return dm_android_granny_call(number, registers);
    }
    case SVC_HOST_PRIVATE_SKILL_AIM: {
        void svc_private_skill_aim(uint64_t *);
        svc_private_skill_aim(registers); return 1;
    }
    case SVC_HOST_PRIVATE_SKILL_RENDER:
        /* Hide only after the Android frontend drew a matching glyph. Its
         * receipt and the original native widget identity must both be live;
         * missing artwork, stale state or no acknowledgement keeps the image. */
#ifdef DM_ANDROID_D2R_UI
        registers[8]=dm_android_hud_hide_glyph(registers[0]);
#else
        registers[8]=0;
#endif
        return 1;
    case SVC_HOST_RIFT_EXIT: {
        void svc_rift_exit(uint64_t *);svc_rift_exit(registers);return 1;
    }
    case SVC_HOST_SAVE_ACT: {
        void svc_save_act(uint64_t *);svc_save_act(registers);return 1;
    }
    case SVC_HOST_MERC_SLOT: {
        void svc_merc_slot(uint64_t *);svc_merc_slot(registers);return 1;
    }
    case SVC_HOST_MERC_SLOT_CL: {
        void svc_merc_slot_cl(uint64_t *);svc_merc_slot_cl(registers);return 1;
    }
    case SVC_HOST_GROUND_ASSET: {
        void svc_ground_asset(uint64_t *);
        svc_ground_asset(registers); return 1;
    }
    case SVC_HOST_DROP_SOUND: {
        void svc_drop_sound(uint64_t *);
        svc_drop_sound(registers); return 1;
    }
    case SVC_HOST_JOIN_SPAWN:
    case SVC_HOST_JOIN_CORPSE: {
        void svc_join_spawn(uint64_t *, int);
        svc_join_spawn(registers, number == SVC_HOST_JOIN_CORPSE); return 1;
    }
    default: return 0;
    }
}
