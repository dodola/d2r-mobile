#include "../host/hle.h"
#include "input_backend.h"
#include "touch_controls_android.h"
#include "../host/dm_input.h"
#include "../ios/Sources/dm_touch_controls.h"
#include <errno.h>
#include <math.h>
#include <stddef.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
static int ready, checks;
static int bridge_reads, bridge_dispatches;
static int bridge_read(void *opaque, dm_android_touch_game_state *state) {
    (void)opaque; ++bridge_reads;
    state->navigation = 1; state->panel_bits = 4; state->belt_rows = 2;
    state->pointer_item = -1; state->target_mask = 0;
    return 1;
}
static void bridge_dispatch(void *opaque, const dm_android_touch_events *events) {
    (void)opaque; (void)events; ++bridge_dispatches;
}
static int source_ready(void *opaque) { (void)opaque; return ready; }
extern const hle_force_ent __start_dm_force[], __stop_dm_force[];
static void *binding(const char *name) {
    for (const hle_force_ent *p=__start_dm_force;p<__stop_dm_force;++p)
        if (!strcmp(p->name,name)) return p->fn;
    die("Input probe missing HID binding %s",name);
}
#define CHECK(x) do {++checks;if(!(x))die("Input fixture failed line=%d",__LINE__);}while(0)
typedef struct { int64_t dt; uint32_t attr,id; int32_t x,y,dx,dy,angle,reserved; } point_state;
typedef struct { int64_t sampling; int32_t count,reserved; point_state points[16]; } touch_state;
_Static_assert(sizeof(touch_state)==0x290,"Touch ABI");
static dm_tc_layout touch_layout;
static int race_mode,race_dispatches;
static int race_read(void *opaque,dm_android_touch_game_state *state) {
    (void)opaque;(void)state;return 0; /* Keep the fixture's explicitly published state. */
}
static void touch_point(float x,float y) {
    int32_t id=3;float nx=x/touch_layout.width,ny=y/touch_layout.height;
    CHECK(!dm_android_touch_event(&id,&nx,&ny,1,0));
    CHECK(!dm_android_touch_event(NULL,NULL,NULL,0,0));
}
static void touch_difficulty(int value) {
    touch_point(touch_layout.diff.x,touch_layout.diff.y);
    touch_point(touch_layout.pick[value-1].x,touch_layout.pick[value-1].y);
}
static void touch_upper_belt(int column) {
    dm_tc_target target=touch_layout.target[DM_TC_BELT1+column];
    touch_point(target.x,target.y-165.0f*touch_layout.height/1320.0f);
}
static void race_dispatch(void *opaque,const dm_android_touch_events *events) {
    (void)opaque;race_dispatches++;
    if(race_mode==1){CHECK(events->diff_request==2);touch_difficulty(3);}
    else if(race_mode==2){CHECK(events->diff_request==2);touch_difficulty(2);}
    else if(race_mode==3){CHECK(events->belt_request==0x110);touch_upper_belt(0);}
    else if(race_mode==4){CHECK(events->world_tap_seq);dm_android_touch_cancel();touch_point(touch_layout.width*.45f,touch_layout.height*.3f);}
}
static void check_touch_event_lifetime(void) {
    dm_android_touch_bind_game_bridge(race_read,race_dispatch,NULL);
    dm_android_touch_game_state state={.belt_rows=4,.pointer_item=-1};
    dm_android_touch_set_game_state(&state);
    dm_android_touch_resize(1280,720,0,0);dm_android_touch_set_enabled(1);
    dm_tc_layout_make(&touch_layout,1280,720);
    dm_android_touch_events before,after;
    /* Dispatching an older request must preserve a new value emitted by
     * the UI callback, including a repeat of the exact same request. */
    touch_difficulty(2);dm_android_touch_get_events(&before);CHECK(before.diff_request==2);
    race_mode=1;
    dm_android_touch_game_tick();dm_android_touch_get_events(&after);
    CHECK(race_dispatches==1&&after.diff_request==3&&after.diff_request_seq!=before.diff_request_seq);
    dm_android_touch_consume_events(&after);dm_android_touch_get_events(&after);CHECK(!after.diff_request);
    touch_difficulty(2);dm_android_touch_get_events(&before);race_mode=2;
    dm_android_touch_game_tick();dm_android_touch_get_events(&after);
    CHECK(race_dispatches==2&&after.diff_request==2&&after.diff_request_seq!=before.diff_request_seq);
    dm_android_touch_consume_events(&after);
    state.belt_open=1;dm_android_touch_set_game_state(&state);
    touch_upper_belt(0);dm_android_touch_get_events(&before);CHECK(before.belt_request==0x110);
    race_mode=3;dm_android_touch_game_tick();dm_android_touch_get_events(&after);
    CHECK(race_dispatches==3&&after.belt_request==0x110&&after.belt_request_seq!=before.belt_request_seq);
    dm_android_touch_consume_events(&after);
    /* Cancelling pending gestures drops every unconsumed event and request.
     * An old acknowledgement must not make a cancelled event pending again. */
    touch_upper_belt(1);
    state.belt_open=0;dm_android_touch_set_game_state(&state);
    touch_difficulty(2);touch_point(touch_layout.width*.45f,touch_layout.height*.3f);
    dm_android_touch_get_events(&before);CHECK(before.diff_request==2&&before.belt_request==0x111&&before.world_tap_seq);
    {void dm_ui_lifecycle(int);dm_ui_lifecycle(1);}
    dm_android_touch_consume_events(&before);dm_android_touch_get_events(&after);
    CHECK(after.cancel_generation!=before.cancel_generation&&!after.diff_request&&!after.belt_request&&!after.world_tap_seq&&!after.mouse_click_seq&&!after.tab_tap_seq&&!after.item_tap_seq&&!after.long_press_seq&&!after.ident_seq&&!after.use_seq);
    /* The old callback can also straddle cancellation plus a new gesture. */
    touch_point(touch_layout.width*.45f,touch_layout.height*.3f);dm_android_touch_get_events(&before);race_mode=4;
    dm_android_touch_game_tick();dm_android_touch_get_events(&after);
    CHECK(race_dispatches==4&&after.cancel_generation!=before.cancel_generation&&after.world_tap_seq&&after.world_tap_seq!=before.world_tap_seq);
    dm_android_touch_consume_events(&after);dm_android_touch_consume_events(&before);dm_android_touch_get_events(&after);CHECK(!after.world_tap_seq);
    dm_android_touch_bind_game_bridge(NULL,NULL,NULL);dm_android_touch_set_enabled(0);
}
int main(int argc,char **argv) {
    if(argc!=2 && argc!=3)return 2;
    int logical=argc==3 && !strcmp(argv[2],"--logical");
    if(argc==3 && !logical)return 2;
    g_t0_ns=now_ns();
    void (*init_touch)(void)=binding("_ZN2nn3hid21InitializeTouchScreenEv");
    void (*init_npad)(void)=binding("_ZN2nn3hid14InitializeNpadEv");
    int (*read_touch)(void *,int)=binding("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi");
    uint32_t (*get_style)(const uint32_t *)=binding("_ZN2nn3hid15GetNpadStyleSetERKj");
    void (*read_pad)(dm_android_npad_state *,const uint32_t *)=binding("_ZN2nn3hid12GetNpadStateEPNS0_16NpadFullKeyStateERKj");
    dm_android_input_config config={.source="device-input-fixture",.panel_width=1280,.panel_height=720,.source_ready=source_ready};
    config.logical_npad_style=logical?1:0;
    CHECK(dm_android_input_initialize_touch()==ENODEV);
    CHECK(dm_android_input_bind(&config)==ENODEV);
    ready=1;CHECK(!dm_android_input_bind(&config));CHECK(dm_android_input_bind(&config)==EALREADY);
    init_touch();init_npad();uint32_t id=0;CHECK(get_style(&id)==(logical?1u:0u));
    /* The title-independent Android bridge is pumped from the same guest HID
     * sampling path as the iOS touch observer.  Verify that a callback can
     * publish a state snapshot without involving JNI or private title offsets. */
    dm_android_touch_bind_game_bridge(bridge_read, bridge_dispatch, NULL);
    dm_android_touch_game_tick();
    dm_android_touch_game_state bridge_state; dm_android_touch_get_game_state(&bridge_state);
    CHECK(bridge_reads==1 && bridge_dispatches==0 && bridge_state.navigation &&
        bridge_state.panel_bits==4 && bridge_state.belt_rows==2);
    dm_android_npad_state pad;read_pad(&pad,&id);CHECK(pad.attributes==(logical?1u:0u)&&pad.buttons==0);
    CHECK(dm_android_input_publish_actions(0,0,0,0,0)==(logical?0:ENODEV));
    touch_state s;CHECK(read_touch(NULL,0)==0);CHECK(dm_android_input_read_touch(NULL,1)==-EINVAL);
    CHECK(read_touch(&s,16)==1&&s.count==0);
    dm_android_pointer p[2]={{.id=99,.x=.25f,.y=.5f},{.id=7,.x=.75f,.y=.2f}};
    CHECK(!dm_android_input_publish_touch(p,2));CHECK(read_touch(&s,16)==1&&s.count==2);
    CHECK(s.points[0].id==0&&s.points[0].attr==1&&s.points[0].x==320&&s.points[0].y==360);
    CHECK(s.points[1].id==1&&s.points[1].attr==1&&s.points[1].x==960&&s.points[1].y==144);
    dm_android_pointer reversed[2]={p[1],p[0]};CHECK(!dm_android_input_publish_touch(reversed,2));
    CHECK(read_touch(&s,1)==1&&s.points[0].id==0&&s.points[0].attr==0&&s.points[1].id==1);
    CHECK(!dm_android_input_publish_touch(p+1,1));CHECK(read_touch(&s,1)==1&&s.count==2&&s.points[0].attr==2&&s.points[1].attr==0);
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].id==1);
    dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    // A complete short tap between samples must retain both edges.
    CHECK(!dm_android_input_publish_touch(p,1));CHECK(!dm_android_input_publish_touch(NULL,0));
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==1);
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    // Reused pointer ID with a second complete tap before the first sample.
    CHECK(!dm_android_input_publish_touch(p,1));CHECK(!dm_android_input_publish_touch(NULL,0));
    p[0].x=.5f;CHECK(!dm_android_input_publish_touch(p,1));CHECK(!dm_android_input_publish_touch(NULL,0));
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==1&&s.points[0].x==320);
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==2&&s.points[0].x==320);
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==1&&s.points[0].x==640);
    CHECK(read_touch(&s,1)==1&&s.points[0].attr==2&&s.points[0].x==640);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    CHECK(!dm_android_input_publish_touch(p,1));dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==0);
    p[0].x=-2;p[0].y=2;CHECK(!dm_android_input_publish_touch(p,1));
    CHECK(read_touch(&s,1)==1&&s.points[0].x==0&&s.points[0].y==719);
    ready=0;CHECK(dm_android_input_publish_touch(p,1)==EAGAIN);
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);CHECK(read_touch(&s,1)==1&&s.count==0);
    ready=1;p[0].x=NAN;CHECK(dm_android_input_publish_touch(p,1)==EINVAL);
    p[0]=p[1];CHECK(dm_android_input_publish_touch(p,2)==EINVAL);
    CHECK(dm_android_input_publish_touch(p,17)==EINVAL);
    CHECK(dm_android_input_set_handheld_mode(3)==EINVAL);
    dm_android_pointer all[16];for(unsigned i=0;i<16;++i)all[i]=(dm_android_pointer){.id=1000+i,.x=1,.y=0};
    CHECK(!dm_android_input_publish_touch(all,16));CHECK(read_touch(&s,1)==1&&s.count==16);
    for(unsigned i=0;i<16;++i)CHECK(s.points[i].id==i&&s.points[i].attr==1&&s.points[i].x==1279&&s.points[i].y==0);
    dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==16);CHECK(read_touch(&s,1)==1&&s.count==0);
    /* Raw NativeActivity touch snapshots must also respect full resets. */
    dm_android_input_status touch_reset;
    dm_android_input_status_get(&touch_reset);dm_android_input_cancel();
    CHECK(dm_android_input_publish_touch_at_generation(all,1,touch_reset.input_resets)==EAGAIN);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    dm_android_input_status_get(&touch_reset);
    CHECK(!dm_android_input_publish_touch_at_generation(all,1,touch_reset.input_resets));
    CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==1);
    dm_android_input_cancel();CHECK(read_touch(&s,1)==1&&s.count==1&&s.points[0].attr==2);
    CHECK(read_touch(&s,1)==1&&s.count==0);
    if(logical){
        /* Physical and touch are independent channels, including short press
         * latches and one non-zero stick taking priority independently. */
        CHECK(!dm_android_input_publish_actions(DM_BTN_A,12000,0,0,0));
        CHECK(!dm_android_input_publish_controller_actions(DM_BTN_B,0,20000,-22000,0));
        CHECK(!dm_android_input_publish_controller_actions(0,0,20000,-22000,0));
        read_pad(&pad,&id);CHECK(pad.buttons==(DM_BTN_A|DM_BTN_B)&&pad.lx==12000&&pad.ly==0&&pad.rx==-22000);
        read_pad(&pad,&id);CHECK(pad.buttons==DM_BTN_A);
        dm_android_input_cancel_touch();read_pad(&pad,&id);CHECK(!pad.buttons&&pad.lx==0&&pad.ly==20000&&pad.rx==-22000);
        /* Touch cancellation/layout resize must preserve a held gamepad key. */
        CHECK(!dm_android_input_publish_controller_buttons(DM_BTN_R));
        dm_android_input_cancel_touch();read_pad(&pad,&id);CHECK(pad.buttons==DM_BTN_R&&pad.ly==20000);
        /* A displaced stick cannot steal control back after a touch, even
         * when unrelated motion callbacks continue to arrive. */
        dm_android_input_touch_activity();CHECK(!dm_android_input_controller_mode());
        CHECK(!dm_android_input_publish_controller_actions(DM_BTN_R,0,20000,-22000,0));
        read_pad(&pad,&id);CHECK(pad.buttons==DM_BTN_R&&!pad.lx&&!pad.ly&&!pad.rx&&!pad.ry);
        CHECK(!dm_android_input_publish_controller_actions(0,0,20000,-22000,0));
        CHECK(!dm_android_input_controller_mode());
        CHECK(!dm_android_input_publish_controller_actions(0,0,0,0,0));
        CHECK(!dm_android_input_publish_controller_actions(0,8192,0,0,0));
        CHECK(dm_android_input_controller_mode());read_pad(&pad,&id);CHECK(pad.lx==8192);
        CHECK(!dm_android_input_publish_controller_actions(0,6553,0,0,0));
        read_pad(&pad,&id);CHECK(pad.lx==6553);
        CHECK(!dm_android_input_publish_controller_actions(0,4914,0,0,0));
        read_pad(&pad,&id);CHECK(!pad.lx);
        CHECK(!dm_android_input_publish_controller_actions(0,6553,0,0,0));
        read_pad(&pad,&id);CHECK(!pad.lx);
        /* Buttons may take over immediately.  A new key, unlike an already
         * held one, can override the post-touch centring requirement. */
        dm_android_input_touch_activity();CHECK(!dm_android_input_controller_mode());
        CHECK(!dm_android_input_publish_controller_actions(DM_BTN_X,20000,0,0,0));
        CHECK(dm_android_input_controller_mode());read_pad(&pad,&id);CHECK(pad.buttons==DM_BTN_X&&pad.lx==20000);
        CHECK(dm_android_input_publish_controller_actions(0,-32768,0,0,0)==EINVAL);
        /* A worker may hold a snapshot across background cancellation.  It
         * must not republish that snapshot after cancellation takes the lock. */
        dm_android_input_status before_reset,after_reset;
        dm_android_input_status_get(&before_reset);
        dm_android_input_cancel();read_pad(&pad,&id);CHECK(!pad.buttons&&!pad.lx&&!pad.ly&&!pad.rx&&!pad.ry);
        dm_android_input_status_get(&after_reset);CHECK(after_reset.input_resets==before_reset.input_resets+1);
        CHECK(dm_android_input_publish_controller_actions_at_generation(DM_BTN_X,20000,0,0,0,before_reset.input_resets)==EAGAIN);
        read_pad(&pad,&id);CHECK(!pad.buttons&&!pad.lx&&!pad.ly&&!pad.rx&&!pad.ry);
        CHECK(!dm_android_input_publish_controller_actions_at_generation(DM_BTN_B,0,0,-22000,0,after_reset.input_resets));
        read_pad(&pad,&id);CHECK(pad.buttons==DM_BTN_B&&pad.rx==-22000);
        dm_android_input_cancel();read_pad(&pad,&id);CHECK(!pad.buttons&&!pad.lx&&!pad.ly&&!pad.rx&&!pad.ry);
        /* A non-zero snapshot is rejected after the route goes away. */
        ready=0;CHECK(dm_android_input_publish_controller_actions(0,20000,0,0,0)==ENODEV);
        ready=1;
        check_touch_event_lifetime();
    }
    FILE *out=fopen(argv[1],"w");CHECK(out);
    fprintf(out,"{\"passed\":true,\"mode\":\"%sinput-fixture\",\"checks\":%d,\"failures\":0,\"sdk_symbol_host_bindings_used\":true,\"original_sdk_hid_body_executed\":false,\"motion_event_injection_checked\":false,\"game_3d_frame_verified\":false}\n",logical?"logical-":"",checks);
    CHECK(!fclose(out));LOG("ANDROID_INPUT_FIXTURE passed checks=%d",checks);return 0;
}
