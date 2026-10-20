/* D2R 3.3.94012 UI adapter. All widget layouts, original calls and interaction
 * policies are shared with iOS; only Android input transport lives here. */
#include "d2r_ui_adapter.h"
#include "d2r_hud_android.h"
#include "d2r_world_overlay_android.h"
#include "d2r_custom_android.h"
#include "d2r_map_reveal.h"
#include "d2r_touch_flow_android.h"
#include "d2r_touch_shell_android.h"
#include "input_backend.h"
#include "../host/dm_cursor.h"
#include "../host/dm_actions.h"
#include "../host/dm_world.h"
#include "../host/dm_input.h"
#include "../host/dm_hud.h"
#include "../host/dm_uiscreen.h"
#include "../host/hle.h"
#include "../ios/Sources/dm_touch_controls.h"
#include <stdatomic.h>
#include <string.h>
#include <math.h>

static int attached;
static uint8_t *main_base;
/* 局外直接复用 iOS 的鼠标消息队列，不把同一触点再送进游戏触摸屏。
 * Front-end taps use the iOS mouse-message queue without duplicating touchscreen input. */
int dm_android_d2r_frontend_touch(int phase,float x,float y) {
    if(!attached || phase<0 || phase>3 || !isfinite(x) || !isfinite(y))return 0;
    if(phase!=3 && dm_uiscreen_ingame())return 0;
    dm_ui_touch(phase,x,y);return 1;
}
extern int dm_android_skill_flow_event(dm_tc_state *,uint32_t,int,int,double) __attribute__((weak));
extern void dm_android_skill_flow_pump(void) __attribute__((weak));
extern void dm_android_d2r_touch_shell_pump(void) __attribute__((weak));
static int title_touch_flow(void *opaque,dm_tc_state *controls,uint32_t generation,int slot,int phase,double now) {
    (void)opaque;
    int defer=dm_android_d2r_touch_flow_event(controls,generation,slot,phase,now);
    if(dm_android_skill_flow_event)defer|=dm_android_skill_flow_event(controls,generation,slot,phase,now);
    return defer;
}
_Atomic int g_dm_l_presses;
/* These diagnostics require the server-side teleport harness, which is not
 * linked into the Android game. Reject their switches at attach time. */
_Atomic int g_dm_seek_tp;
int g_dm_seek_tp_x,g_dm_seek_tp_y;

uint64_t dm_input_stick_activity(void);
uint64_t dm_stick_last_ns(void) { return dm_input_stick_activity(); }
void dm_gfx_shot_at(long frame,const char *path) {
    (void)frame;(void)path;
    die("DM_TEST_WORLD_TAP_SHOT requires the desktop screenshot harness");
}

int dm_android_d2r_ui_attach(module_t *main) {
    if(attached)return 1;
    if(!main || !main->base || main->size<0x3211700 ||
       strcmp(main->build_id,"db0507f36b84200ac05f75d76ed5026f") ||
       !gate_obs_present(OBS_UIMSG) || !gate_obs_present(OBS_TICK) || !gate_obs_present(OBS_C2S) ||
       !gate_svc_present(SVC_HOST_C2S_DELIVER))return 0;
    if(getenv("DM_TEST_WORLD_SEEK") || getenv("DM_TEST_WORLD_TAP_SHOT"))
        die("Android UI bridge does not include desktop teleport/screenshot diagnostics");
    if(!dm_android_custom_attach(main))return 0;
    main_base=main->base;attached=1;
    dm_android_map_attach(main);
    dm_android_touch_bind_flow(title_touch_flow,NULL);
    LOG("ANDROID_D2R_UI_BRIDGE attached=1 source=shared-ios widget_tree=1 events=1 input_filter=1");
    return 1;
}

void dm_android_d2r_ui_message(const char *category,const char *action,const char *argument) {
    if(!attached)return;
    dm_ui_on_uimsg(category,action,argument);
    { void dm_update_on_uimsg(const char *,const char *,const char *); dm_update_on_uimsg(category,action,argument); }
    if(!strcmp(category,"PanelManager")){dm_android_hud_message(action,argument);dm_android_world_overlay_message(action,argument);}
    void dm_kills_on_uimsg(const char *,const char *,const char *);
    void dm_vault_on_uimsg(const char *,const char *,const char *);
    void dm_pouch_on_uimsg(const char *,const char *,const char *);
    void dm_shoprefresh_on_uimsg(const char *,const char *,const char *,uint8_t *);
    dm_kills_on_uimsg(category,action,argument);
    dm_vault_on_uimsg(category,action,argument);
    dm_pouch_on_uimsg(category,action,argument);
    dm_shoprefresh_on_uimsg(category,action,argument,main_base);
}

void dm_android_d2r_ui_c2s(uint64_t game,uint64_t player,const uint8_t *packet,uint32_t length) {
    if(!attached || !packet)return;
    void dm_shoprefresh_c2s(uint64_t,uint64_t,const uint8_t *,uint32_t);
    dm_shoprefresh_c2s(game,player,packet,length);
}

void dm_android_title_touch_begin(int target) {
    if(attached && target==DM_TC_PORTAL) { void dm_tp_request(void);dm_tp_request(); }
}

void dm_android_d2r_ui_tick(uint64_t game,uint64_t player,uint8_t *main) {
    if(!attached || !main)return;
    void dm_freetp_server_step(uint8_t *,uint64_t,uint64_t);
    void dm_elysium_stash_step(uint64_t,uint64_t,uint8_t *);
    void dm_pages_tick(uint64_t,uint64_t,uint8_t *);
    void dm_vault_tick(uint64_t,uint64_t,uint8_t *);
    void dm_autobuff_srv_tick(uint8_t *,uint64_t,uint64_t);
    void dm_pouch_srv_tick(uint8_t *,uint64_t,uint64_t);
    void dm_kills_tick(uint64_t,uint64_t,uint8_t *);
    dm_freetp_server_step(main,game,player);
    if(player)dm_elysium_stash_step(game,player,main);
    dm_pages_tick(game,player,main);
    dm_vault_tick(game,player,main);
    if(player)dm_autobuff_srv_tick(main,game,player);
    dm_pouch_srv_tick(main,game,player);
    if(player)dm_kills_tick(game,player,main);
}

void dm_android_d2r_ui_pump(dm_android_touch_game_state *state) {
    if(!attached || !state)return;
    dm_ui_frontend_pump();
    if(dm_android_skill_flow_pump)dm_android_skill_flow_pump();
    dm_android_custom_scope(1);dm_actions_pump();dm_android_custom_scope(0);
    { void dm_padmode_request(int);dm_padmode_request(dm_android_input_controller_mode()); }
    if(dm_android_d2r_touch_shell_pump)dm_android_d2r_touch_shell_pump();
    dm_world_pump();
    dm_android_map_pump();
    dm_ui_mouse_pump();
    dm_android_d2r_touch_flow_pump();
    float x=0,y=0;
    if(dm_android_touch_pointer(&x,&y,NULL)) {
        dm_cursor_set_screen(x,y,1,1);
    } else dm_cursor_clear();
    /* Item classification belongs to the gesture's origin query, rather
     * than whichever widget the moving pointer happens to cross this poll. */
    state->pointer_item=-1;
    state->panel_bits=dm_ui_panel_state();
    state->belt_rows=dm_belt_rows(&state->belt_open);
    state->holding=dm_hud_held()!=0;
    state->tap_box=dm_ui_tap_box();
    state->cur_stack=dm_ui_cur_stack_id();
    state->hover_unid=dm_ui_hover_unid();
    dm_uiscreen_set_panel(state->navigation && state->navigation!=3);
}

void dm_android_d2r_ui_events(void *opaque,const dm_android_touch_events *events) {
    (void)opaque;
    if(!attached || !events)return;
    if(events->world_tap_seq)dm_ui_world_tap(events->world_x,events->world_y,events->world_x,events->world_y);
    if(events->mouse_click_seq) { dm_ui_touch(0,events->mouse_x,events->mouse_y);dm_ui_touch(2,events->mouse_x,events->mouse_y); }
    if(events->tab_tap_seq)dm_ui_tab_go();
    if(events->item_tap_seq)dm_ui_item_tap();
    if(events->long_press_seq)dm_ui_long_press();
    if(events->ident_seq)dm_ui_identify_hovered();
    if(events->use_seq)dm_ui_use_item();
    if(events->stack_take_seq)dm_stash_take(1);
    if(events->diff_request)dm_hud_request_players(events->diff_request);
    int belt=events->belt_request;
    if(belt==1 || belt==2)dm_belt_expand(belt==1);
    else if(belt)dm_belt_use(belt&15,(belt>>4)&15);
}

/* Called after input_backend has released its lock. Cursor and mode filters
 * can safely query input state and must operate on the sample actually sent. */
void dm_android_title_input_filter(dm_android_npad_state *state,uint64_t generation) {
    static uint64_t deferred,previous,previous_generation;
    if(!attached || !state)return;
    /* Use the generation captured with this sample, rather than querying
     * current status: a reset can happen after the backend releases its lock. */
    if(generation!=previous_generation || !state->attributes) {
        deferred=previous=0;previous_generation=generation;
    }
    if(!state->attributes)return;
    uint64_t dm_ui_alone_take(uint64_t);
    int dm_padmode_input_gate(uint64_t *);
    uint64_t dm_runeword_pad_filter(uint64_t),dm_padmode_page_filter(uint64_t);
    uint64_t merged=(state->buttons & ~dm_ui_auto_release()) | dm_ui_auto_buttons() | deferred;
    uint64_t alone=dm_ui_alone_take(merged);
    if(alone) { deferred|=state->buttons;state->buttons=alone; }
    else { state->buttons=merged;deferred=0; }
    if(dm_padmode_input_gate(&state->buttons))state->lx=state->ly=state->rx=state->ry=0;
    state->buttons=dm_runeword_pad_filter(state->buttons);
    state->buttons=dm_padmode_page_filter(state->buttons);
    state->buttons=dm_swapskill_filter(state->buttons);
    dm_cursor_apply(&state->buttons,&state->lx,&state->ly);
    if((state->buttons & DM_BTN_L) && !(previous & DM_BTN_L))atomic_fetch_add(&g_dm_l_presses,1);
    previous=state->buttons;
}
