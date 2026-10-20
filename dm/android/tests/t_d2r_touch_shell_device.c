#include "../d2r_touch_shell_android.h"
#include "../d2r_host_modules.h"
#include "../input_backend.h"
#include "../../host/dm.h"
#include "../../host/dm_input.h"
#include <math.h>
#include <string.h>
#include <unistd.h>

struct dm_flags g_flag;uint64_t g_t0_ns;
static unsigned checks;
#define CHECK(x) do{++checks;if(!(x))die("touch shell fixture line=%d",__LINE__);}while(0)
static int pumping,free_cursor=1,full,bind,touch,pending,busy,world_result=-1,scene_nav;
static uint32_t ui=1;static uint64_t scene=1,world=1;static unsigned world_seq,world_asked,full_on,full_off;
static float asked_x,asked_y;
static float mini[4]={.6f,.08f,.9f,.35f};
static int ready(void *p){(void)p;return 1;}
uint32_t dm_ui_gen(void){CHECK(pumping);return ui;}
int dm_padmode_busy(void){CHECK(pumping);return busy;}
int dm_ui_free_cursor(void){CHECK(pumping);return free_cursor;}
int dm_ui_map_is_full(void){CHECK(pumping);return full;}
int dm_ui_minimap_rect(float r[4]){CHECK(pumping);memcpy(r,mini,sizeof mini);return 1;}
int dm_ui_bind_active(void){CHECK(pumping);return bind;}
int dm_ui_touch_active(void){CHECK(pumping);return touch;}
int dm_cursor_pending(void){return pending;} /* portable transport may read this atomic-only guard */
void dm_ui_hand_idle(int idle){(void)idle;}
void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *s){CHECK(pumping);*s=(dm_android_d2r_scene_status){.attached=1,.game=1,.navigation=scene_nav,.scene_epoch=scene,.world_epoch=world};}
unsigned dm_world_tap(float x,float y){CHECK(pumping);world_asked++;asked_x=x;asked_y=y;world_result=-1;return ++world_seq;}
int dm_world_tap_result(unsigned seq){CHECK(pumping);return seq==world_seq?world_result:-1;}
void dm_ui_map_full(int on){CHECK(pumping);full=on!=0;if(on)full_on++;else full_off++;}
static int event(void *p,dm_tc_state *s,uint32_t g,int slot,int phase,double now){(void)p;dm_android_d2r_touch_shell_event(s,g,slot,phase,now);return 0;}
static dm_android_touch_snapshot snap(void){dm_android_touch_snapshot s;dm_android_touch_snapshot_get(&s);return s;}
static void pump(void){pumping=1;dm_android_d2r_touch_shell_pump();pumping=0;}
static void contact(float x,float y){int32_t id=7;CHECK(!dm_android_touch_event(&id,&x,&y,1,0));}
static void lift(float x,float y){CHECK(!dm_android_touch_release(7,x,y));CHECK(!dm_android_touch_event(NULL,NULL,NULL,0,0));}
static void tap(float x,float y){contact(x,y);lift(x,y);}
static void mode(int nav){dm_android_touch_cancel();scene++;scene_nav=nav;dm_android_touch_game_state g={.navigation=nav,.belt_rows=4,.pointer_item=-1};dm_android_touch_set_game_state(&g);pump();}
static int parked(void){dm_android_touch_snapshot s=snap();return s.controls.pointer_valid&&fabsf(s.controls.pointer_x/s.controls.layout.width-.02f)<.001f&&fabsf(s.controls.pointer_y/s.controls.layout.height-.5f)<.001f;}
static void item(dm_tc_state *s,void *p){dm_tc_pointer_item(s,*(int *)p);}
static void classify(int kind){dm_android_touch_snapshot s=snap();CHECK(dm_android_touch_edit(s.cancel_generation,item,&kind));}
static void held(dm_tc_state *s,void *p){s->holding=*(int *)p;}
static void holding(int on){dm_android_touch_snapshot s=snap();CHECK(dm_android_touch_edit(s.cancel_generation,held,&on));}
static int bridge_read(void *opaque,dm_android_touch_game_state *state){(void)opaque;(void)state;return 0;}
static void bridge_dispatch(void *opaque,const dm_android_touch_events *events){(void)opaque;(void)events;}
int main(void){
    g_t0_ns=now_ns();dm_android_input_config config={.source="touch-shell-fixture",.panel_width=1280,.panel_height=720,.source_ready=ready,.logical_npad_style=1,.logical_npad_id=0};
    CHECK(!dm_android_input_bind(&config)&&!dm_android_input_initialize_npad());
    dm_android_touch_bind_game_bridge(bridge_read,bridge_dispatch,NULL);
    dm_android_touch_resize(1280,720,0,0);dm_android_touch_bind_flow(event,NULL);dm_android_touch_set_enabled(1);
    mode(0);unsigned taps=snap().controls.world_tap_seq;
    tap(.7f,.10f);CHECK(snap().controls.world_tap_seq==taps);pump();CHECK(world_asked==1&&fabsf(asked_x-.7f)<.001f&&fabsf(asked_y-.1f)<.001f&&!full_on);
    world_result=2;pump();CHECK(!full_on); /* NPC interacted, map unchanged. */
    tap(.7f,.10f);pump();world_result=0;pump();CHECK(full_on==1&&full);pump(); /* host applies the queued map request before its next observation. */
    tap(.7f,.10f);pump();CHECK(full_off==1&&!full);pump();
    tap(.7f,.2f);pump();CHECK(world_asked==2&&snap().controls.world_tap_seq==taps+1); /* map body remains world. */
    contact(.7f,.1f);contact(.8f,.1f);lift(.8f,.1f);pump();CHECK(world_asked==2); /* moved strip contact. */
    tap(.7f,.1f);pump();world_result=0;dm_android_touch_cancel();pump();CHECK(full_on==1);
    tap(.7f,.1f);pump();world_result=0;world++;pump();CHECK(full_on==1); /* stale world answer. */
    pump();full=1;pump();tap(.5f,.2f);pump();CHECK(full_off==2&&!full); /* full map blank tap exits. */
    full=1;pump();contact(.15f,.7f);CHECK(snap().controls.joy_slot>=0);lift(.15f,.7f);pump();CHECK(full_off==2);full=0;
    /* 原地图键覆盖小地图时仍可开关全屏，拖技能到同一区域仍取消施法。
     * Minimap taps work under the former map key, while dragging a skill there still cancels it. */
    mode(0);dm_android_touch_snapshot ms=snap();
    CHECK(!(ms.controls.enabled_targets&(1u<<DM_TC_MAP)));
    float mx=ms.controls.layout.target[DM_TC_MAP].x/ms.controls.layout.width;
    float my=ms.controls.layout.target[DM_TC_MAP].y/ms.controls.layout.height;
    mini[0]=mx-.06f;mini[1]=my-.01f;mini[2]=1;mini[3]=my+.3f;pump();
    unsigned requests=world_asked,on=full_on,off=full_off;
    tap(mx,my);CHECK(!(snap().controls.buttons&DM_BTN_MINUS));pump();CHECK(world_asked==requests+1);
    world_result=0;pump();CHECK(full_on==on+1&&full);pump();
    tap(mx,my);pump();CHECK(full_off==off+1&&!full);pump();
    float sx=ms.controls.layout.target[DM_TC_SKILL1].x/ms.controls.layout.width;
    float sy=ms.controls.layout.target[DM_TC_SKILL1].y/ms.controls.layout.height;
    contact(sx,sy);contact(mx,my);CHECK(snap().controls.map_cancel_active&&snap().controls.finger[0].aim_cancelled);
    lift(mx,my);pump();CHECK(world_asked==requests+1&&!snap().controls.map_cancel_active);
    mode(1);CHECK(!parked());usleep(320000);pump();CHECK(parked()); /* default belt focus yields. */
    free_cursor=0;mode(1);usleep(320000);pump();CHECK(!parked());free_cursor=1;pump();usleep(320000);bind=1;pump();CHECK(!parked());bind=0;touch=1;pump();CHECK(!parked());touch=0;pending=1;pump();CHECK(!parked());pending=0;holding(1);pump();CHECK(!parked());holding(0);pump();CHECK(parked());
    /* Plain item taps keep their descriptions until another touch. */
    mode(1);contact(.45f,.35f);classify(1);lift(.45f,.35f);pump();usleep(330000);pump();CHECK(!parked());
    /* Ordinary mouse taps hide tooltip after the short post-click grace. */
    contact(.4f,.3f);classify(0);lift(.4f,.3f);pump();usleep(70000);pump();CHECK(parked());
    mode(1);busy=1;usleep(320000);pump();CHECK(!parked());busy=0;pump();usleep(320000);pump();CHECK(parked());
    printf("ANDROID_D2R_TOUCH_SHELL_OK checks=%u world_before_map=1 tooltip_park=1 default_belt_focus_yields=1 cancellation_scope=1 host_calls_only_in_pump=1 original_game_ui_executed=0\n",checks);
    return 0;
}
