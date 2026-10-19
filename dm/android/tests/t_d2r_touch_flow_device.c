/* Production Android touch transport + portable iOS gestures + title flow.
 * Host answers are recorded fixtures, never original game/widget callees. */
#include "../d2r_touch_flow_android.h"
#include "../d2r_host_modules.h"
#include "../input_backend.h"
#include "../../host/dm.h"
int dm_ui_shelf_grid(float r[4]){(void)r;return 0;}
int dm_ui_stack_left(void){return 0;}
#include "../../host/dm_cursor.h"
#include <math.h>
#include <unistd.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x))die("Touch flow fixture line=%d",__LINE__);} while(0)
static int pumping,kind,bag=1,busy;
static int cancel_during_drag,cancel_during_answer,release_during_drop;
static uint32_t ui_generation=1;
static uint64_t world_epoch=1;
extern void dm_guest_frame_tick(void);
static unsigned query_seq,done_seq,origin_queries,drop_queries;
static unsigned drag_begin,drag_move,drag_end,buys,takes,cubes,sells,classifications;
static int drag_kind;
static float query_x,query_y,query_finger,buy_x,buy_y;
static float drop_x=.73f,drop_y=.62f;
static double physical_end,portable_end;
static void lift(float x,float y);
static int ready(void *opaque){(void)opaque;return 1;}
uint32_t dm_ui_gen(void){CHECK(pumping);return ui_generation;}
int dm_padmode_busy(void){CHECK(pumping);return busy;}
void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *s){CHECK(pumping);*s=(dm_android_d2r_scene_status){.attached=1,.game=1,.world_epoch=world_epoch};}
unsigned dm_ui_item_query(float x,float y){CHECK(pumping);origin_queries++;query_x=x;query_y=y;return ++query_seq;}
unsigned dm_ui_drop_query(float x,float y,float finger){CHECK(pumping);drop_queries++;query_x=x;query_y=y;query_finger=finger;unsigned seq=++query_seq;if(release_during_drop){release_during_drop=0;lift(.12f,.12f);}return seq;}
int dm_ui_item_result(unsigned seq){CHECK(pumping);if(cancel_during_answer){cancel_during_answer=0;dm_android_touch_cancel();}return seq==done_seq?kind:-1;}
int dm_ui_drop_bag(void){CHECK(pumping);return bag;}
int dm_ui_drop_point(float *x,float *y){CHECK(pumping);*x=drop_x;*y=drop_y;return 1;}
int dm_ui_origin_point(float *x,float *y){CHECK(pumping);*x=.42f;*y=.36f;return 1;}
void dm_ui_drag_lift(float y){CHECK(pumping && y>0 && y<.5f);}
void dm_ui_drag_begin(int k,float x,float y,float tolerance){CHECK(pumping && tolerance>0 && x>=0 && y>=0);drag_begin++;drag_kind=k;if(cancel_during_drag){cancel_during_drag=0;dm_android_touch_cancel();}}
void dm_ui_drag_move(float x,float y){CHECK(pumping && x>=0 && y>=0);drag_move++;}
void dm_ui_drag_end(void){CHECK(pumping);drag_end++;}
void dm_ui_touch(int phase,float x,float y){CHECK(pumping && phase==3 && x==0 && y==0);}
void dm_ui_shop_buy_at(float x,float y){CHECK(pumping);buys++;buy_x=x;buy_y=y;}
void dm_ui_stack_take_at(float x,float y){CHECK(pumping && x>=0 && y>=0);takes++;}
void dm_ui_stack_cube_at(float x,float y){CHECK(pumping && x>=0 && y>=0);cubes++;}
void dm_shop_sell_cursor(void){CHECK(pumping);sells++;}
void dm_android_skill_flow_classified(uint32_t generation,int slot,double contact_start){CHECK(pumping && generation && slot>=0 && contact_start>0);classifications++;}
static int event(void *opaque,dm_tc_state *s,uint32_t generation,int slot,int phase,double now){
    (void)opaque;
    if(phase==DM_ATF_RELEASE)physical_end=now;
    if(phase==DM_ATF_END)portable_end=now;
    return dm_android_d2r_touch_flow_event(s,generation,slot,phase,now);
}
static dm_android_touch_snapshot snap(void){dm_android_touch_snapshot s;dm_android_touch_snapshot_get(&s);return s;}
static void same_frame_pump(void){pumping=1;dm_android_d2r_touch_flow_pump();pumping=0;}
static void pump(void){dm_guest_frame_tick();same_frame_pump();}
static void answer(int result){kind=result;done_seq=query_seq;}
static void contact(float x,float y){int32_t id=9;CHECK(!dm_android_touch_event(&id,&x,&y,1,0));}
static void lift(float x,float y){CHECK(!dm_android_touch_release(9,x,y));CHECK(!dm_android_touch_event(NULL,NULL,NULL,0,0));}
static void reset(void){dm_android_touch_cancel();pump();physical_end=portable_end=0;}
static void classified(int result){contact(.45f,.35f);unsigned before=origin_queries;pump();CHECK(origin_queries==before+1);answer(result);pump();}
static void drop(int result,int inventory){
    bag=inventory;classified(1);contact(.6f,.55f);pump();
    lift(.72f,.68f);answer(0);pump();answer(result);pump();
}
static void stale_drop_timeout(int origin,int prior) {
    reset();unsigned bought=buys,sold=sells,taken=takes,cubed=cubes;
    bag=1;classified(origin);contact(.6f,.55f);pump();answer(prior);pump();
    CHECK(!portable_end);lift(.12f,.12f);unsigned queried=drop_queries;pump();
    CHECK(drop_queries==queried+1 && fabsf(query_x-.12f)<.001f && fabsf(query_finger-.12f)<.001f);
    usleep(260000);pump();dm_android_touch_snapshot s=snap();
    CHECK(s.controls.pointer_slot<0 && portable_end==physical_end);
    CHECK(buys==bought && sells==sold && takes==taken && cubes==cubed);
    if(origin==1) {
        CHECK(s.controls.pointer_pulse_pending==2 && fabsf(s.controls.pointer_pending_x/1280-.42f)<.001f &&
              fabsf(s.controls.pointer_pending_y/720-.36f)<.001f);
    } else CHECK(!s.controls.pulse_button && !s.controls.pointer_pulse_pending);
    answer(prior);pump();CHECK(buys==bought && sells==sold && takes==taken && cubes==cubed);
}
int main(int argc,char **argv){
    if(argc!=2)return 2;g_t0_ns=now_ns();
    dm_android_input_config config={.source="touch-flow-fixture",.panel_width=1280,.panel_height=720,
        .source_ready=ready,.logical_npad_style=1,.logical_npad_id=0};
    CHECK(!dm_android_input_bind(&config) && !dm_android_input_initialize_npad());
    dm_android_touch_resize(1280,720,0,0);
    dm_android_touch_game_state game={.navigation=1,.belt_rows=4,.pointer_item=-1};
    dm_android_touch_set_game_state(&game);dm_android_touch_bind_flow(event,NULL);dm_android_touch_set_enabled(1);

    contact(.45f,.35f);contact(.6f,.55f);
    dm_android_touch_snapshot s=snap();int slot=s.controls.pointer_slot;
    CHECK(slot>=0 && !s.controls.finger[slot].pointer_dragged && !s.controls.finger[slot].ui_drag);
    pump();CHECK(fabsf(query_x-.45f)<.001f && fabsf(query_y-.35f)<.001f);
    answer(1);pump();s=snap();CHECK(s.controls.finger[slot].pointer_dragged && s.controls.pulse_button);
    lift(.72f,.68f);s=snap();CHECK(s.controls.finger[slot].active && !portable_end);
    answer(0);pump();CHECK(fabsf(query_x-.72f)<.001f && fabsf(query_finger-.68f)<.001f && query_y<query_finger);
    answer(5);pump();s=snap();CHECK(!s.controls.finger[slot].active && portable_end==physical_end);
    CHECK(s.controls.pointer_pulse_pending==2 && fabsf(s.controls.pointer_pending_x/1280-drop_x)<.001f && fabsf(s.controls.pointer_pending_y/720-drop_y)<.001f);

    /* A release before origin classification is not an unclassified click. */
    reset();unsigned before=classifications;contact(.45f,.35f);pump();lift(.45f,.35f);
    CHECK(!portable_end);answer(7);pump();s=snap();CHECK(portable_end==physical_end && !s.controls.mouse_click_seq && classifications==before+1);
    reset();unsigned longs=snap().controls.long_press_seq;contact(.45f,.35f);pump();lift(.45f,.35f);usleep(150000);answer(1);pump();
    CHECK(snap().controls.long_press_seq==longs && portable_end==physical_end);
    reset();contact(.45f,.35f);pump();usleep(550000);lift(.45f,.35f);answer(1);pump();
    CHECK(snap().controls.long_press_seq==longs+1 && portable_end==physical_end);

    reset();classified(0);contact(.45f,.6f);CHECK(!drag_begin);pump();CHECK(drag_begin==1 && drag_kind==1 && drag_move);
    unsigned ended=drag_end;lift(.45f,.62f);pump();CHECK(drag_end==ended && snap().controls.pointer_slot>=0);
    same_frame_pump();CHECK(drag_end==ended);pump();CHECK(drag_end>ended);
    reset();classified(11);contact(.7f,.35f);pump();CHECK(drag_kind==2);lift(.7f,.35f);pump();
    reset();unsigned dragged=drag_begin;classified(6);contact(.45f,.6f);pump();CHECK(drag_begin==dragged);lift(.45f,.6f);pump();
    reset();contact(.45f,.35f);contact(.45f,.6f);pump();lift(.45f,.62f);answer(0);
    ended=drag_end;dragged=drag_begin;pump();CHECK(drag_begin==dragged+1 && drag_end==ended && !portable_end);
    same_frame_pump();CHECK(drag_end==ended);pump();CHECK(drag_end>ended && portable_end==physical_end);

    /* Origin-scoped trade rules: only inventory accepts shelf/stack drops. */
    reset();unsigned bought=buys;classified(7);contact(.6f,.55f);pump();lift(.72f,.68f);answer(0);pump();bag=0;answer(5);pump();CHECK(buys==bought);
    reset();classified(7);contact(.6f,.55f);pump();lift(.72f,.68f);answer(0);pump();bag=1;answer(5);pump();CHECK(buys==bought+1 && fabsf(buy_x-.45f)<.001f && fabsf(buy_y-.35f)<.001f);
    reset();classified(8);contact(.6f,.55f);pump();lift(.72f,.68f);answer(0);pump();answer(5);pump();CHECK(takes==1);
    reset();classified(8);contact(.6f,.55f);pump();lift(.72f,.68f);answer(0);pump();answer(11);pump();CHECK(cubes==1);
    reset();drop(7,1);CHECK(sells==1);
    /* Previously answered landing zones cannot authorize a later UP which
     * jumps elsewhere without MOVE and whose final query never completes. */
    stale_drop_timeout(1,7);stale_drop_timeout(7,5);stale_drop_timeout(8,5);
    stale_drop_timeout(8,11);stale_drop_timeout(1,5);
    /* Publish/query-apply race: a physical UP during an older query still
     * requires a new query for its coordinates, even if the old answer buys. */
    reset();bought=buys;classified(7);contact(.6f,.55f);release_during_drop=1;pump();
    CHECK(physical_end && fabsf(query_x-.6f)<.001f);answer(5);before=drop_queries;pump();
    CHECK(drop_queries==before+1 && fabsf(query_x-.12f)<.001f && !portable_end && buys==bought);
    answer(0);pump();CHECK(portable_end==physical_end && buys==bought);

    reset();contact(.45f,.35f);pump();uint32_t cancelled=snap().cancel_generation;dm_android_touch_cancel();answer(1);pump();s=snap();CHECK(s.cancel_generation!=cancelled && s.controls.pointer_slot<0 && !s.controls.pulse_button);
    reset();classified(1);contact(.6f,.55f);pump();ui_generation++;pump();CHECK(snap().controls.pointer_slot<0);
    reset();classified(1);world_epoch++;pump();CHECK(snap().controls.pointer_slot<0);
    reset();classified(1);dm_android_input_cancel();pump();CHECK(snap().controls.pointer_slot<0);
    reset();classified(0);contact(.45f,.6f);ended=drag_end;cancel_during_drag=1;pump();
    CHECK(snap().controls.pointer_slot<0 && drag_end>ended);
    reset();contact(.45f,.35f);pump();answer(1);before=classifications;cancel_during_answer=1;pump();
    CHECK(snap().controls.pointer_slot<0 && classifications==before && !snap().controls.pulse_button);
    cancelled=snap().cancel_generation;dm_android_touch_cancel();contact(.45f,.35f);
    CHECK(!dm_android_touch_cancel_at_generation(cancelled) && snap().controls.pointer_slot>=0);
    reset();contact(.45f,.35f);pump();lift(.45f,.35f);usleep(260000);pump();s=snap();CHECK(s.controls.pointer_slot<0 && !s.controls.pulse_button);
    reset();classified(1);contact(.6f,.55f);pump();lift(.72f,.68f);usleep(260000);pump();s=snap();CHECK(s.controls.pointer_slot<0 && s.controls.pointer_pulse_pending==2);
    CHECK(fabsf(s.controls.pointer_pending_x/1280-.42f)<.001f && fabsf(s.controls.pointer_pending_y/720-.36f)<.001f);
    FILE *out=fopen(argv[1],"w");CHECK(out);
    fprintf(out,"{\"passed\":true,\"checks\":%u,\"fixture_only\":true,\"production_transport_and_flow\":true,\"origin_query\":true,\"release_wait\":true,\"physical_release_timing\":true,\"final_release_coordinates\":true,\"scroll_slider\":true,\"quick_swipe_frame_handoff\":true,\"inventory_drop\":true,\"trade_destination_gate\":true,\"final_drop_confirmation_required\":true,\"stale_trade_timeout_cases\":5,\"release_during_query\":true,\"late_drop_reply_ignored\":true,\"cancel_ui_world_input_generations\":true,\"cancellation_during_host_calls\":true,\"host_calls_only_in_game_pump\":true,\"original_game_ui_executed\":false,\"app_save_data_used\":false}\n",checks);
    CHECK(!fclose(out));return 0;
}
