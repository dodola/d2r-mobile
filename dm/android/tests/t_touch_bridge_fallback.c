/* Exercise the production adapter with complete and incomplete title transports. */
#include "../touch_controls_android.h"
#include "../input_backend.h"
#include "../../host/dm.h"
#include <math.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
static unsigned checks,flow_calls;
static int safe_left_units,safe_right_units;
void dm_ui_safe_left(int units){safe_left_units=units;}
void dm_ui_safe_right(int units){safe_right_units=units;}
#define CHECK(x) do {++checks;if(!(x))die("touch bridge fallback fixture line=%d",__LINE__);}while(0)
static int ready(void *opaque){(void)opaque;return 1;}
static int read_state(void *opaque,dm_android_touch_game_state *state){(void)opaque;(void)state;return 0;}
static void dispatch(void *opaque,const dm_android_touch_events *events){(void)opaque;(void)events;}
static int flow(void *opaque,dm_tc_state *state,uint32_t generation,int slot,int phase,double now){
    (void)opaque;(void)state;(void)generation;(void)slot;(void)phase;(void)now;++flow_calls;return 0;
}
static dm_android_touch_snapshot snapshot(void){dm_android_touch_snapshot s;dm_android_touch_snapshot_get(&s);return s;}
static int safe(dm_android_touch_snapshot s){
    if(s.controls.layout.target[DM_TC_PORTAL].r || s.controls.layout.diff.r || s.controls.belt_rows!=1 || s.controls.belt_open || s.controls.diff_open)return 0;
    if(s.controls.enabled_targets&(1u<<DM_TC_PORTAL))return 0;
    for(int i=0;i<8;i++)if(s.controls.layout.pick[i].r)return 0;
    return 1;
}
int main(void){
    g_t0_ns=now_ns();
    dm_android_input_config config={.source="touch-bridge-fallback-fixture",.panel_width=1280,.panel_height=720,
        .source_ready=ready,.logical_npad_style=1,.logical_npad_id=0};
    CHECK(!dm_android_input_bind(&config)&&!dm_android_input_initialize_npad());
    dm_android_touch_resize(1280,720,0,0);dm_android_touch_set_enabled(1);
    CHECK(safe(snapshot()));
    dm_android_touch_game_state fake={.belt_rows=4,.belt_open=1,.pointer_item=-1,.target_mask=(1u<<DM_TC_COUNT)-1};
    dm_android_touch_set_game_state(&fake);CHECK(safe(snapshot()));
    dm_android_touch_bind_game_bridge(read_state,NULL,NULL);CHECK(safe(snapshot()));
    dm_android_touch_bind_flow(flow,NULL);
    dm_android_touch_bind_game_bridge(read_state,dispatch,NULL);
    dm_android_touch_snapshot s=snapshot();CHECK(s.controls.layout.target[DM_TC_PORTAL].r>0);CHECK(s.controls.layout.diff.r>0);
    for(int i=0;i<8;i++)CHECK(s.controls.layout.pick[i].r>0);
    CHECK(s.controls.belt_rows==4);CHECK(s.controls.enabled_targets&(1u<<DM_TC_PORTAL));
    dm_android_touch_resize(1000,600,0,0);CHECK(snapshot().controls.layout.target[DM_TC_PORTAL].r>0);
    s=snapshot();int32_t id=1;
    float x=s.controls.layout.joy_x/s.controls.layout.width,y=s.controls.layout.joy_y/s.controls.layout.height;
    CHECK(!dm_android_touch_event(&id,&x,&y,1,0));CHECK(snapshot().controls.joy_slot>=0);
    x+=s.controls.layout.joy_radius/s.controls.layout.width;
    CHECK(!dm_android_touch_event(&id,&x,&y,1,0));
    dm_android_npad_state pad;CHECK(!dm_android_input_read_npad(&pad,0,1)&&pad.lx);
    uint32_t generation=snapshot().cancel_generation;
    dm_android_touch_bind_game_bridge(NULL,NULL,NULL);s=snapshot();CHECK(safe(s));CHECK(s.cancel_generation>generation);
    CHECK(s.controls.joy_slot==DM_TC_NONE&&!s.controls.buttons&&!s.controls.lx&&!s.controls.ly);
    CHECK(!dm_android_input_read_npad(&pad,0,1)&&!pad.buttons&&!pad.lx&&!pad.ly&&!pad.rx&&!pad.ry);
    for(int i=0;i<16;i++)CHECK(!s.controls.finger[i].active);
    unsigned before=flow_calls;CHECK(!dm_android_touch_event(&id,&x,&y,1,0));CHECK(flow_calls==before);
    dm_android_touch_bind_game_bridge(read_state,dispatch,NULL);CHECK(snapshot().controls.layout.target[DM_TC_PORTAL].r>0);
    fake.navigation=1;dm_android_touch_set_game_state(&fake);CHECK(snapshot().controls.navigation_mode);
    /* A navigation button owns its contact: emit Npad only, never a second
     * raw guest touchscreen press which could select/skip a dialogue too. */
    CHECK(!dm_android_input_initialize_touch());
    for(int target=DM_TC_MAIN;target<=DM_TC_SKILL4;++target){
        s=snapshot();id=target+10;
        x=s.controls.layout.target[target].x/s.controls.layout.width;
        y=s.controls.layout.target[target].y/s.controls.layout.height;
        CHECK(!dm_android_touch_event(&id,&x,&y,1,0));
        CHECK(snapshot().controls.finger[0].target==target);
        uint64_t touch[0x290/8];
        CHECK(dm_android_input_read_touch(touch,1)==1 && touch[1]==0);
        CHECK(!dm_android_input_read_npad(&pad,0,1) && pad.buttons==s.controls.layout.target[target].button);
        CHECK(!dm_android_touch_event(&id,&x,&y,1,0));
        CHECK(!dm_android_input_read_npad(&pad,0,1) && !pad.buttons);
        CHECK(dm_android_input_read_touch(touch,1)==1 && touch[1]==0);
        CHECK(!dm_android_touch_release(id,x,y));
        CHECK(!dm_android_touch_event(NULL,NULL,NULL,0,0));
        CHECK(!dm_android_input_read_npad(&pad,0,1) && !pad.buttons);
        CHECK(dm_android_input_read_touch(touch,1)==1 && touch[1]==0);
    }
    /* Wide output and a cutout on either side must share the exact drawing /
     * normalized hit geometry, without stretching button radii. A resize also
     * releases a held button before replacing its target positions. */
    fake.navigation=0;fake.panel_bits=0;fake.belt_open=0;dm_android_touch_set_game_state(&fake);
    float radius=0;
    for(int side=0;side<3;side++){
        float left=side==1?48:0,right=side==2?48:0;
        dm_android_touch_resize(side?1000:800,450,left,right);s=snapshot();
        CHECK(safe_left_units==(int)roundf(left*2160/450));
        CHECK(safe_right_units==(int)roundf(right*2160/450));
        if(side)CHECK(s.controls.layout.target[DM_TC_MAIN].r==radius);
        radius=s.controls.layout.target[DM_TC_MAIN].r;
        CHECK(s.controls.layout.joy_x-s.controls.layout.joy_radius>=left);
        for(int target=DM_TC_MAIN;target<=DM_TC_SKILL4;target++){
            dm_tc_target t=s.controls.layout.target[target];
            CHECK(t.x-t.r>=left && t.x+t.r<=s.controls.layout.width-right);
            CHECK(t.y-t.r>=0 && t.y+t.r<=s.controls.layout.height);
            x=t.x/s.controls.layout.width;y=t.y/s.controls.layout.height;
            CHECK(dm_android_touch_hit(x,y)==target);
            id=30+target;CHECK(!dm_android_touch_event(&id,&x,&y,1,0));
            CHECK(snapshot().controls.finger[0].target==target);
            CHECK(!dm_android_input_read_npad(&pad,0,1));
            if(target==DM_TC_MAIN)CHECK(pad.buttons==t.button);
            CHECK(!dm_android_touch_event(NULL,NULL,NULL,0,0));
        }
        id=50;x=s.controls.layout.target[DM_TC_MAIN].x/s.controls.layout.width;
        y=s.controls.layout.target[DM_TC_MAIN].y/s.controls.layout.height;
        CHECK(!dm_android_touch_event(&id,&x,&y,1,0));
        dm_android_touch_resize(960,450,0,0);
        CHECK(!dm_android_input_read_npad(&pad,0,1) && !pad.buttons);
    }
    dm_android_touch_bind_game_bridge(read_state,NULL,NULL);CHECK(safe(snapshot()));CHECK(!snapshot().controls.navigation_mode);
    printf("ANDROID_TOUCH_BRIDGE_FALLBACK_OK checks=%u safe_legacy=1 full_rebind=1 unbind_cancels=1 private_flow_retired=1\n",checks);
    return 0;
}
