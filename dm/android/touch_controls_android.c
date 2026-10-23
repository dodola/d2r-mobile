/* Android adapter for the unchanged upstream portable touch state machine.
 * No game-version addresses or Apple UI/skill observation hooks are linked. */
#include "../ios/Sources/dm_touch_controls.h"
#include "input_backend.h"
#include "touch_controls_android.h"
#include "../host/dm.h"
#include "../host/dm_hud.h"
#include "../host/dm_custom.h"
#include <jni.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <string.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static dm_tc_state controls;
static struct {int active,id;} slots[16];
static int ready,enabled;
static uint64_t pulse_epoch,pulse_samples;
static double pulse_start,pulse_seen;
static uint64_t last_buttons;
static int32_t last_axes[4];
static int published;
static int access_slot=-1;
static double access_until;
static dm_android_touch_game_state game_state;
static uint32_t target_mask;
static dm_android_touch_events event_cursor;
static uint32_t cancel_generation=1, diff_request_seq, belt_request_seq;
static dm_android_touch_flow_fn flow_callback;
static void *flow_opaque;
static double pending_end_time[16];
static int publish(void);
static void cancel(void);
static void cancel_for_reason(int reason);
static int flow_event(int slot,int phase,double now) {
    return flow_callback ? flow_callback(flow_opaque,&controls,cancel_generation,slot,phase,now) : 0;
}
static struct {
    dm_android_touch_read_state_fn read_state;
    dm_android_touch_dispatch_events_fn dispatch_events;
    void *opaque;
} game_bridge;
extern void dm_touch_game_set_scene(int game,uint64_t scene_epoch,uint64_t world_epoch) __attribute__((weak));
extern void dm_android_title_touch_begin(int target) __attribute__((weak));
extern int dm_cursor_pending(void) __attribute__((weak));
extern void dm_ui_hand_idle(int idle) __attribute__((weak));
static int observed_game, observed_area=-1;
static uint64_t observed_scene_epoch=1, observed_world_epoch=1;

/* The full iOS cursor pump reads private UIKit widget trees. Android exposes
 * the same lifecycle/UI generation contract through this portable bridge;
 * title-specific observers can call the touch state APIs below without
 * linking the Darwin cursor implementation. */
__attribute__((weak)) void dm_ui_gen_bump(int why) { (void)why; }
__attribute__((weak)) void dm_ui_lifecycle(int background) { if(background)dm_android_touch_cancel();LOG("Android UI lifecycle background=%d",background!=0); }

static double seconds(void) {return now_ns()/1e9;}
typedef struct { int difficulty,belt; } pending_requests;
static pending_requests request_write_begin(void) {
    pending_requests old={controls.diff_request,controls.belt_request};
    controls.diff_request=controls.belt_request=0;
    return old;
}
static void request_write_end(pending_requests old) {
    /* The portable state machine exposes request values, not request
     * sequence numbers. Clearing temporarily lets us distinguish a new
     * request even when it has the same value as the pending one. */
    if(controls.diff_request)++diff_request_seq;else controls.diff_request=old.difficulty;
    if(controls.belt_request)++belt_request_seq;else controls.belt_request=old.belt;
}
static void control_begin(int slot,float x,float y,double now) {
    pending_requests old=request_write_begin();dm_tc_begin_at(&controls,slot,x,y,now);request_write_end(old);
    pending_end_time[slot]=0;flow_event(slot,DM_ATF_BEGIN,now);
    if(!controls.navigation_mode && dm_android_title_touch_begin)
        dm_android_title_touch_begin(controls.finger[slot].target);
}
static void control_finish(int slot,double now) {
    flow_event(slot,DM_ATF_END,now);
    pending_requests old=request_write_begin();dm_tc_end_at(&controls,slot,now);request_write_end(old);
    pending_end_time[slot]=0;flow_event(slot,DM_ATF_AFTER_END,now);
}
static void control_end(int slot,double now) {
    if(flow_event(slot,DM_ATF_RELEASE,now)) {pending_end_time[slot]=now;return;}
    control_finish(slot,now);
}
static void control_tick(double now) {
    if(controls.pointer_slot>=0 && pending_end_time[controls.pointer_slot]>0)
        now=fmin(now,pending_end_time[controls.pointer_slot]);
    pending_requests old=request_write_begin();dm_tc_tick(&controls,now);request_write_end(old);
}
static void restrict_layout(void) {
    uint32_t mask;
    if (!game_bridge.read_state || !game_bridge.dispatch_events) {
        /* Legacy 91738 and unknown builds have no verified private UI
         * transport. Keep the main branch's usable Npad controls while
         * hiding actions which require that transport to do anything. */
        controls.layout.target[DM_TC_PORTAL].r=0;
        controls.layout.diff.r=0;
        for(int i=0;i<8;i++)controls.layout.pick[i].r=0;
        controls.belt_rows=1;
        controls.belt_open=controls.diff_open=0;
        mask=((1u<<DM_TC_COUNT)-1)&~(1u<<DM_TC_PORTAL);
    } else {
        controls.belt_rows=game_state.belt_rows>0?game_state.belt_rows:4;
        mask=target_mask;
        if (!mask) mask=game_state.navigation ? dm_tc_nav_mask(!(game_state.panel_bits & 4)) : ((1u<<DM_TC_COUNT)-1);
        /* 与 iOS 一致，使用小地图手势；保留右上几何供拖技能取消。
         * Match iOS: use minimap gestures, retaining the geometry for skill-drag cancellation. */
        mask&=~(1u<<DM_TC_MAP);
    }
    if(ready && controls.enabled_targets!=mask)cancel_for_reason(DM_ATF_RESET_SCENE);
    dm_tc_enable_targets(&controls,mask);
}

int dm_android_touch_hit(float x, float y) {
    pthread_mutex_lock(&lock);
    int hit=ready ? dm_tc_hit(&controls.layout,x*controls.layout.width,y*controls.layout.height) : DM_TC_NONE;
    pthread_mutex_unlock(&lock);
    return hit;
}

void dm_android_touch_set_target_mask(uint32_t mask) {
    pthread_mutex_lock(&lock); target_mask=mask;
    if (ready) { restrict_layout(); }
    pthread_mutex_unlock(&lock);
}

void dm_android_touch_set_game_state(const dm_android_touch_game_state *state) {
    if (!state) return;
    pthread_mutex_lock(&lock);
    game_state=*state;
    target_mask=game_state.target_mask;
    dm_tc_set_panel_state(game_state.panel_bits);
    if (ready) {
        if(controls.navigation_mode!=(game_state.navigation!=0))cancel_for_reason(DM_ATF_RESET_SCENE);
        dm_tc_set_navigation_mode(&controls,game_state.navigation);
    }
    controls.belt_rows=game_state.belt_rows>0?game_state.belt_rows:1;
    controls.belt_open=game_state.belt_open!=0;
    if (ready) {
        dm_tc_set_holding(&controls,game_state.holding!=0);
        dm_tc_set_tap_box(game_state.tap_box!=0);
        dm_tc_set_cur_stack(game_state.cur_stack);
        dm_tc_set_hover_unid(game_state.hover_unid);
        if (game_state.pointer_item>=0 && controls.pointer_slot>=0)
            dm_tc_pointer_item(&controls,game_state.pointer_item);
        restrict_layout();
    }
    pthread_mutex_unlock(&lock);
}

void dm_android_touch_get_game_state(dm_android_touch_game_state *state) {
    if (!state) return; pthread_mutex_lock(&lock); *state=game_state; pthread_mutex_unlock(&lock);
}

int dm_android_touch_pointer(float *x,float *y,int *query_item) {
    pthread_mutex_lock(&lock);
    int valid=ready && enabled && controls.pointer_valid && controls.layout.width>0 && controls.layout.height>0;
    if(valid) { if(x)*x=controls.pointer_x/controls.layout.width; if(y)*y=controls.pointer_y/controls.layout.height; }
    if(query_item)*query_item=valid && controls.pointer_slot>=0;
    pthread_mutex_unlock(&lock);
    return valid;
}

void dm_android_touch_get_events(dm_android_touch_events *events) {
    if (!events) return; memset(events,0,sizeof *events);
    pthread_mutex_lock(&lock);
    if (controls.world_tap_seq!=event_cursor.world_tap_seq && controls.layout.width>0 && controls.layout.height>0) { events->world_tap_seq=controls.world_tap_seq; events->world_x=controls.world_tap_x/controls.layout.width; events->world_y=controls.world_tap_y/controls.layout.height; }
    if (controls.mouse_click_seq!=event_cursor.mouse_click_seq && controls.layout.width>0 && controls.layout.height>0) { events->mouse_click_seq=controls.mouse_click_seq; events->mouse_x=controls.mouse_click_x/controls.layout.width; events->mouse_y=controls.mouse_click_y/controls.layout.height; }
    if (controls.tab_tap_seq!=event_cursor.tab_tap_seq) events->tab_tap_seq=controls.tab_tap_seq;
    if (controls.item_tap_seq!=event_cursor.item_tap_seq) events->item_tap_seq=controls.item_tap_seq;
    if (controls.long_press_seq!=event_cursor.long_press_seq) events->long_press_seq=controls.long_press_seq;
    if (controls.ident_seq!=event_cursor.ident_seq) events->ident_seq=controls.ident_seq;
    if (controls.use_seq!=event_cursor.use_seq) events->use_seq=controls.use_seq;
    if (controls.stack_take_seq!=event_cursor.stack_take_seq) events->stack_take_seq=controls.stack_take_seq;
    events->diff_request=controls.diff_request; events->belt_request=controls.belt_request;
    events->cancel_generation=cancel_generation;
    events->diff_request_seq=diff_request_seq; events->belt_request_seq=belt_request_seq;
    pthread_mutex_unlock(&lock);
}

void dm_android_touch_consume_events(const dm_android_touch_events *events) {
    if (!events) return; pthread_mutex_lock(&lock);
    if(events->cancel_generation!=cancel_generation){pthread_mutex_unlock(&lock);return;}
    if (events->world_tap_seq==controls.world_tap_seq) event_cursor.world_tap_seq=events->world_tap_seq;
    if (events->mouse_click_seq==controls.mouse_click_seq) event_cursor.mouse_click_seq=events->mouse_click_seq;
    if (events->tab_tap_seq==controls.tab_tap_seq) event_cursor.tab_tap_seq=events->tab_tap_seq;
    if (events->item_tap_seq==controls.item_tap_seq) event_cursor.item_tap_seq=events->item_tap_seq;
    if (events->long_press_seq==controls.long_press_seq) event_cursor.long_press_seq=events->long_press_seq;
    if (events->ident_seq==controls.ident_seq) event_cursor.ident_seq=events->ident_seq;
    if (events->use_seq==controls.use_seq) event_cursor.use_seq=events->use_seq;
    if (events->stack_take_seq==controls.stack_take_seq) event_cursor.stack_take_seq=events->stack_take_seq;
    if (events->diff_request && events->diff_request_seq==diff_request_seq) controls.diff_request=0;
    if (events->belt_request && events->belt_request_seq==belt_request_seq) controls.belt_request=0;
    pthread_mutex_unlock(&lock);
}

void dm_android_touch_bind_game_bridge(dm_android_touch_read_state_fn read_state,
                                       dm_android_touch_dispatch_events_fn dispatch_events,
                                       void *opaque) {
    pthread_mutex_lock(&lock);
    if(game_bridge.read_state==read_state && game_bridge.dispatch_events==dispatch_events && game_bridge.opaque==opaque) {
        pthread_mutex_unlock(&lock);return;
    }
    cancel_for_reason(DM_ATF_RESET_SCENE);
    game_bridge.read_state=read_state; game_bridge.dispatch_events=dispatch_events; game_bridge.opaque=opaque;
    if(!read_state || !dispatch_events) {
        /* The old title flow must not keep consuming gestures after its
         * private event transport has been removed. */
        flow_callback=NULL;flow_opaque=NULL;controls.mouse_click_mode=0;
        memset(&game_state,0,sizeof game_state);game_state.pointer_item=-1;target_mask=0;
        dm_tc_set_panel_state(0);
        if(ready)dm_tc_set_navigation_mode(&controls,0);
    }
    if(ready) {
        /* Recreate radii hidden by the safe layout before enabling a
         * verified bridge, including binding after the first resize. */
        dm_tc_resize(&controls,controls.layout.width,controls.layout.height);
        restrict_layout();
    }
    pthread_mutex_unlock(&lock);
}

void dm_android_touch_game_tick(void) {
    dm_android_touch_read_state_fn read_state;
    dm_android_touch_dispatch_events_fn dispatch_events;
    void *opaque;
    pthread_mutex_lock(&lock);
    read_state=game_bridge.read_state; dispatch_events=game_bridge.dispatch_events; opaque=game_bridge.opaque;
    pthread_mutex_unlock(&lock);
    /* The D2R binding observer needs a scene/world generation before its
     * private gate is allowed to learn a HUD cohort.  Derive that generation
     * from the already-published portable HUD snapshot; titles without the
     * optional observer simply leave this weak seam unused. */
    if (dm_touch_game_set_scene && !read_state) {
        dm_hud_info hud; dm_hud_get(&hud);
        int game=hud.valid!=0, area=game?hud.area:-1;
        if (game!=observed_game) { observed_game=game; observed_area=area; observed_scene_epoch++; observed_world_epoch++; }
        else if (game && area!=observed_area) { observed_area=area; observed_world_epoch++; }
        dm_touch_game_set_scene(game,observed_scene_epoch,observed_world_epoch);
    }
    if (read_state) {
        dm_android_touch_game_state state;
        memset(&state,0,sizeof state); state.pointer_item=-1;
        if (read_state(opaque,&state)) dm_android_touch_set_game_state(&state);
    }
    if (dispatch_events) {
        dm_android_touch_events events;
        dm_android_touch_get_events(&events);
        if (events.world_tap_seq || events.mouse_click_seq || events.tab_tap_seq || events.item_tap_seq ||
            events.long_press_seq || events.ident_seq || events.use_seq || events.diff_request || events.belt_request || events.stack_take_seq) {
            dispatch_events(opaque,&events);
            dm_android_touch_consume_events(&events);
        }
    }
}

static int publish(void) {
    int32_t axes[4]={controls.lx,controls.ly,controls.rx,controls.ry};
    if(published && controls.buttons==last_buttons && !memcmp(axes,last_axes,sizeof axes))return 0;
    int rc=dm_android_input_publish_actions(controls.buttons,axes[0],axes[1],axes[2],axes[3]);
    if(!rc){last_buttons=controls.buttons;memcpy(last_axes,axes,sizeof axes);published=1;
        static unsigned logged;if(logged++<128)LOG("ANDROID_UPSTREAM_TOUCH buttons=%#llx axes=%d,%d,%d,%d set=%d",(unsigned long long)controls.buttons,axes[0],axes[1],axes[2],axes[3],controls.skill_set);}
    return rc;
}
static void cancel_for_reason(int reason) {
    if(ready)dm_tc_cancel_all(&controls);
    ++cancel_generation;
    memset(pending_end_time,0,sizeof pending_end_time);
    flow_event(reason,DM_ATF_RESET,seconds());
    /* Sequence counters in the portable state survive gesture cancellation.
     * Move the consumer to those counters, and invalidate acknowledgements
     * which a game callback may still hold from before this cancellation. */
    event_cursor.world_tap_seq=controls.world_tap_seq;
    event_cursor.mouse_click_seq=controls.mouse_click_seq;
    event_cursor.tab_tap_seq=controls.tab_tap_seq;
    event_cursor.item_tap_seq=controls.item_tap_seq;
    event_cursor.long_press_seq=controls.long_press_seq;
    event_cursor.ident_seq=controls.ident_seq;
    event_cursor.use_seq=controls.use_seq;
    event_cursor.stack_take_seq=controls.stack_take_seq;
    controls.diff_request=controls.belt_request=0;
    memset(slots,0,sizeof slots);pulse_epoch=0;published=0;
    access_slot=-1;
    dm_android_input_cancel_touch();
}
static void cancel(void) {cancel_for_reason(DM_ATF_RESET_EXPLICIT);}
static void apply_layout_setting(void) {
    int dm_android_title_custom_settings(void) __attribute__((weak));
    if(!dm_android_title_custom_settings || !dm_android_title_custom_settings())return;
    int above=dm_custom_get("touch_swap_above")!=0;
    if(controls.layout.swap_above==above)return;
    float width=controls.layout.width,height=controls.layout.height;
    if(ready)cancel();
    dm_tc_set_swap_above(above);
    if(ready){dm_tc_resize(&controls,width,height);restrict_layout();}
}
static void tick(void) {
    if(!ready || !enabled)return;
    apply_layout_setting();
    double now=seconds();dm_android_input_status input;dm_android_input_status_get(&input);
    if(access_slot>=0 && now>=access_until){control_end(access_slot,now);access_slot=-1;}
    control_tick(now);
    if(controls.pulse_button) {
        if(pulse_epoch!=controls.pulse_epoch){pulse_epoch=controls.pulse_epoch;pulse_samples=input.npad_samples;pulse_start=now;pulse_seen=0;}
        if(!pulse_seen && input.npad_samples>pulse_samples)pulse_seen=now;
        double hold=fmax(0.12,controls.pulse_hold_s);
        /* Direct navigation-button taps use a negative hold as a one-sample
         * pulse.  A/B must be released before a dialog can consume the next
         * menu layer; otherwise one finger tap can select two layers. */
        int one_sample = controls.navigation_mode && controls.pulse_hold_s < 0;
        if((one_sample && input.npad_samples>pulse_samples) ||
           (!one_sample && pulse_seen && now-pulse_seen>=hold && input.npad_samples-pulse_samples>=3 && (!dm_cursor_pending || !dm_cursor_pending())) ||
           now-pulse_start>2.0+hold) {
            dm_tc_pulse_end(&controls,pulse_epoch);pulse_samples=input.npad_samples;pulse_start=now;
        }
    }else if(dm_tc_pulse_pending(&controls) && (input.npad_samples-pulse_samples>=2 || now-pulse_start>0.5))dm_tc_pulse_resume(&controls);
    if(dm_ui_hand_idle) { int active=0;for(int i=0;i<16;i++)active|=controls.finger[i].active;
        dm_ui_hand_idle(!active && !controls.pulse_button && !dm_tc_pulse_pending(&controls) && (!dm_cursor_pending || !dm_cursor_pending())); }
    (void)publish();
}
void dm_android_touch_resize(float w,float h,float left,float right) {
    if(!isfinite(w)||!isfinite(h)||w<=0||h<=0)return;
    pthread_mutex_lock(&lock);dm_tc_set_tall_scale(1);dm_tc_set_safe_insets(left,right);
    /* Keep the game's top-corner HUD inside the same cutout safe area as
     * our controls. The title adapter consumes layout units (2160 high). */
    extern void dm_ui_safe_left(int units) __attribute__((weak));
    extern void dm_ui_safe_right(int units) __attribute__((weak));
    if(dm_ui_safe_left)dm_ui_safe_left(isfinite(left)&&left>0?(int)fminf(800,roundf(left*2160/h)):0);
    if(dm_ui_safe_right)dm_ui_safe_right(isfinite(right)&&right>0?(int)fminf(800,roundf(right*2160/h)):0);
    if(ready){cancel();dm_tc_resize(&controls,w,h);}else{dm_tc_init(&controls,w,h);ready=1;}
    apply_layout_setting();
    controls.mouse_click_mode=flow_callback!=NULL;
    restrict_layout();
    const dm_tc_layout *l=&controls.layout;
    LOG("ANDROID_CONTROLS_LAYOUT viewport=%.3fx%.3f safe=%.3f,%.3f main=%.3f,%.3f,r%.3f joy=%.3f,%.3f,r%.3f belt1=%.3f,%.3f",
        (double)w,(double)h,(double)left,(double)right,
        (double)l->target[DM_TC_MAIN].x,(double)l->target[DM_TC_MAIN].y,(double)l->target[DM_TC_MAIN].r,
        (double)l->joy_x,(double)l->joy_y,(double)l->joy_radius,
        (double)l->target[DM_TC_BELT1].x,(double)l->target[DM_TC_BELT1].y);
    pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeResize(JNIEnv *env,jclass cls,jfloat w,jfloat h,jfloat left,jfloat right) {
    (void)env;(void)cls;dm_android_touch_resize(w,h,left,right);
}
void dm_android_touch_set_enabled(int on) {
    pthread_mutex_lock(&lock);cancel();enabled=on!=0;if(on)dm_android_input_touch_activity();pthread_mutex_unlock(&lock);
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeEnable(JNIEnv *env,jclass cls,jboolean on) {
    (void)env;(void)cls;dm_android_touch_set_enabled(on);
}
void dm_android_touch_cancel(void) {pthread_mutex_lock(&lock);cancel();pthread_mutex_unlock(&lock);}
int dm_android_touch_cancel_at_generation(uint32_t expected) {
    pthread_mutex_lock(&lock);int current=cancel_generation==expected;
    if(current)cancel();pthread_mutex_unlock(&lock);return current;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeCancel(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;dm_android_touch_cancel();
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeEvent(JNIEnv *env,jclass cls,jintArray ids,jfloatArray xs,jfloatArray ys,jboolean cancelled) {
    (void)cls;if(!ids||!xs||!ys)return EINVAL;
    int n=(*env)->GetArrayLength(env,ids);
    if(n>16||(*env)->GetArrayLength(env,xs)!=n||(*env)->GetArrayLength(env,ys)!=n)return EINVAL;
    jint id[16];jfloat x[16],y[16];
    (*env)->GetIntArrayRegion(env,ids,0,n,id);(*env)->GetFloatArrayRegion(env,xs,0,n,x);(*env)->GetFloatArrayRegion(env,ys,0,n,y);
    if((*env)->ExceptionCheck(env))return EINVAL;
    return dm_android_touch_event(id,x,y,n,cancelled);
}
int dm_android_touch_event(const int32_t *id,const float *x,const float *y,int n,int cancelled) {
    if(n<0 || n>16 || (n && (!id || !x || !y)))return EINVAL;
    for(int i=0;i<n;i++){if(!isfinite(x[i])||!isfinite(y[i]))return EINVAL;for(int j=0;j<i;j++)if(id[i]==id[j])return EINVAL;}
    pthread_mutex_lock(&lock);
    if(cancelled){cancel();pthread_mutex_unlock(&lock);return 0;}
    if(!ready||!enabled){pthread_mutex_unlock(&lock);return ENODEV;}
    if(n)dm_android_input_touch_activity();
    double now=seconds();
    for(int slot=0;slot<16;slot++)if(slots[slot].active){int seen=0;for(int i=0;i<n;i++)seen|=slots[slot].id==id[i];
        if(!seen){control_end(slot,now);slots[slot].active=0;}}
    dm_android_pointer raw[16];int nr=0;
    for(int i=0;i<n;i++) {
        int slot=0;while(slot<16&&(!slots[slot].active||slots[slot].id!=id[i]))slot++;
        float px=x[i]*controls.layout.width,py=y[i]*controls.layout.height;
        if(slot==16){slot=0;while(slot<16&&(slots[slot].active||controls.finger[slot].active))slot++;if(slot==16){pthread_mutex_unlock(&lock);return EOVERFLOW;}
            slots[slot].active=1;slots[slot].id=id[i];control_begin(slot,px,py,now);
        }else {dm_tc_move_at(&controls,slot,px,py,now);flow_event(slot,DM_ATF_MOVE,now);}
        if(controls.finger[slot].target==DM_TC_NONE)raw[nr++]=(dm_android_pointer){.id=(uint32_t)id[i],.x=x[i],.y=y[i]};
    }
    /* A committed layout change can cancel all contacts during tick. Do not
     * republish the raw contacts collected before that cancellation. */
    uint32_t before_tick=cancel_generation;tick();if(cancel_generation!=before_tick)nr=0;
    int rc=dm_android_input_publish_touch(raw,(size_t)nr);if(!rc)rc=publish();
    pthread_mutex_unlock(&lock);return rc;
}
void dm_android_touch_bind_flow(dm_android_touch_flow_fn callback,void *opaque) {
    pthread_mutex_lock(&lock);cancel();flow_callback=callback;flow_opaque=opaque;
    controls.mouse_click_mode=callback!=NULL;pthread_mutex_unlock(&lock);
}
void dm_android_touch_snapshot_get(dm_android_touch_snapshot *out) {
    if(!out)return;
    pthread_mutex_lock(&lock);*out=(dm_android_touch_snapshot){.controls=controls,
        .cancel_generation=cancel_generation,.ready=ready,.enabled=enabled};pthread_mutex_unlock(&lock);
}
int dm_android_touch_edit(uint32_t generation,dm_android_touch_edit_fn callback,void *opaque) {
    if(!callback)return 0;
    pthread_mutex_lock(&lock);int current=cancel_generation==generation;
    if(current) {callback(&controls,opaque);if(ready && enabled)(void)publish();}
    pthread_mutex_unlock(&lock);return current;
}
int dm_android_touch_finish_pending(uint32_t generation,int slot,double end_time) {
    if(slot<0 || slot>=16)return 0;
    pthread_mutex_lock(&lock);
    int current=cancel_generation==generation && controls.finger[slot].active && pending_end_time[slot]>0;
    if(current) {control_finish(slot,end_time);if(ready && enabled)(void)publish();}
    pthread_mutex_unlock(&lock);return current;
}
int dm_android_touch_release(int32_t id,float x,float y) {
    if(!isfinite(x)||!isfinite(y))return EINVAL;
    pthread_mutex_lock(&lock);
    for(int slot=0;slot<16;slot++)if(slots[slot].active && slots[slot].id==id) {
        double now=seconds();dm_tc_move_at(&controls,slot,x*controls.layout.width,y*controls.layout.height,now);
        flow_event(slot,DM_ATF_MOVE,now);control_end(slot,now);slots[slot].active=0;
        break;
    }
    tick();int rc=ready && enabled ? publish() : 0;pthread_mutex_unlock(&lock);return rc;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeRelease(JNIEnv *env,jclass cls,jint id,jfloat x,jfloat y) {
    (void)env;(void)cls;return dm_android_touch_release(id,x,y);
}
int dm_android_touch_visual_state(int *skill_set,int *diff_open,uint64_t *buttons) {
    pthread_mutex_lock(&lock);
    if(skill_set)*skill_set=controls.skill_set;
    if(diff_open)*diff_open=controls.diff_open;
    if(buttons)*buttons=controls.buttons;
    int active=enabled && ready;pthread_mutex_unlock(&lock);return active;
}
JNIEXPORT jfloatArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeFrame(JNIEnv *env,jclass cls) {
    (void)cls;jfloat data[58]={0};pthread_mutex_lock(&lock);tick();
    for(int i=0;i<DM_TC_COUNT;i++){dm_tc_target t=controls.layout.target[i];data[i*3]=t.x;data[i*3+1]=t.y;data[i*3+2]=t.r;
        for(int j=0;j<16;j++)if(controls.finger[j].active&&controls.finger[j].target==i)data[44+i]=1;}
    data[36]=controls.layout.joy_x;data[37]=controls.layout.joy_y;data[38]=controls.layout.joy_radius;
    data[39]=controls.layout.swap.x;data[40]=controls.layout.swap.y;data[41]=controls.layout.swap.r;
    data[42]=data[36]+controls.lx/32767.f*data[38];data[43]=data[37]-controls.ly/32767.f*data[38];
    data[56]=(float)controls.skill_set;data[57]=controls.joy_slot!=DM_TC_NONE;
    pthread_mutex_unlock(&lock);jfloatArray out=(*env)->NewFloatArray(env,58);if(out)(*env)->SetFloatArrayRegion(env,out,0,58,data);return out;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeActivate(JNIEnv *env,jclass cls,jint target) {
    (void)env;(void)cls;pthread_mutex_lock(&lock);
    if(ready&&enabled){int slot=15;while(slot>=0&&(slots[slot].active||controls.finger[slot].active))slot--;
        if(slot>=0){double now=seconds();float x,y;
            if(target>=0&&target<DM_TC_COUNT){x=controls.layout.target[target].x;y=controls.layout.target[target].y;}
            else if(target==12){x=controls.layout.swap.x;y=controls.layout.swap.y;}
            else {x=controls.layout.joy_x;y=controls.layout.joy_y;}
            control_begin(slot,x,y,now);
            if(target>=20&&target<=23){float radius=controls.layout.joy_radius;dm_tc_move_at(&controls,slot,x+(target==20?-radius:target==21?radius:0),y+(target==22?-radius:target==23?radius:0),now);access_slot=slot;access_until=now+0.25;}
            else control_end(slot,now+0.01);
            tick();
        }
    }
    pthread_mutex_unlock(&lock);
}

JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeGameState(JNIEnv *env,jclass cls,
    jint navigation,jint panelBits,jint beltRows,jint beltOpen,jint holding,jint pointerItem,
    jint tapBox,jint curStack,jint hoverUnid,jint mask) {
    (void)env; (void)cls;
    dm_android_touch_game_state state={.navigation=navigation!=0,.panel_bits=panelBits,
        .belt_rows=beltRows,.belt_open=beltOpen!=0,.holding=holding!=0,
        .pointer_item=pointerItem,.tap_box=tapBox!=0,.cur_stack=(uint32_t)curStack,
        .hover_unid=(uint32_t)hoverUnid,.target_mask=(uint32_t)mask};
    dm_android_touch_set_game_state(&state);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeHit(JNIEnv *env,jclass cls,jfloat x,jfloat y) {
    (void)env; (void)cls; return dm_android_touch_hit(x,y);
}

JNIEXPORT jintArray JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeTouchGameState(JNIEnv *env,jclass cls) {
    (void)cls;
    dm_android_touch_game_state state;
    memset(&state,0,sizeof state); state.pointer_item=-1;
    dm_android_touch_get_game_state(&state);
    jint values[10] = {
        state.navigation, state.panel_bits, state.belt_rows, state.belt_open,
        state.holding, state.pointer_item, state.tap_box,
        (jint)state.cur_stack, (jint)state.hover_unid, (jint)state.target_mask
    };
    jintArray result=(*env)->NewIntArray(env,10);
    if (result) (*env)->SetIntArrayRegion(env,result,0,10,values);
    return result;
}
