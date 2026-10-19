/* Real Android transport + shared portable controls + skill flow + generated
 * native glyph resolver. Host observations/callees are controlled fixtures;
 * this never opens the game, calls its widgets, or accesses player saves. */
#include "../d2r_touch_skill_flow_android.h"
#include "../d2r_touch_flow_android.h"
#include "../d2r_host_modules.h"
#include "../input_backend.h"
#include "../../host/dm.h"
int dm_ui_shelf_grid(float r[4]){(void)r;return 0;}
int dm_ui_stack_left(void){return 0;}
#include "../../host/dm_cursor.h"
#include "../../host/dm_actions.h"
#include "../../host/dm_input.h"
#include "../../ios/Sources/dm_touch_native_art.h"
#include <math.h>
#include <unistd.h>

struct dm_flags g_flag;
uint64_t g_t0_ns;
static unsigned checks;
#define CHECK(x) do {++checks;if(!(x))die("Skill flow fixture line=%d",__LINE__);}while(0)
static int pumping,game=1,navigation=1,bind_state,busy,unstable,hit_kind,focus_skill=-1;
static uint32_t ui=1,hit_id,focus_id;
static uint64_t scene=1,world=1;
static dm_tg_snapshot native;
static dm_points_info points;
static unsigned stats[4],skills,binds,cancels,point_reads;
static unsigned reset_at_read;
static uint64_t last_bind;
static int native_fresh=1;
static int integrate_drag;
static unsigned query_sequence;
static float hit_x=.45f,hit_y=.35f;

static int ready(void *p){(void)p;return 1;}
uint32_t dm_ui_gen(void){CHECK(pumping);return ui;}
int dm_ui_unstable(void){CHECK(pumping);return unstable;}
int dm_padmode_busy(void){CHECK(pumping);return busy;}
int dm_ui_bind_active(void){CHECK(pumping);return bind_state;}
int dm_cursor_pending(void){return 0;}
void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *o){
    CHECK(pumping);*o=(dm_android_d2r_scene_status){.attached=1,.game=game,
        .navigation=navigation,.scene_epoch=scene,.world_epoch=world};
}
int dm_touch_game_snapshot(dm_tg_snapshot *o){
    CHECK(pumping);*o=native;if(native_fresh)o->captured_ns=now_ns();return 1;
}
void dm_points_get(dm_points_info *o){
    CHECK(pumping);*o=points;point_reads++;
    if(reset_at_read && point_reads==reset_at_read)dm_android_touch_cancel();
}
int dm_ui_bind_skill_hit(float *x,float *y){CHECK(pumping);*x=hit_x;*y=hit_y;return hit_kind;}
uint32_t dm_ui_hit_id(void){CHECK(pumping);return hit_id;}
int dm_ui_hit_selected_skill(uint32_t expected){
    CHECK(pumping);return hit_id==expected && focus_id==expected?focus_skill:-1;
}
void dm_stat_add(int stat,int n){CHECK(pumping && stat>=0 && stat<4 && n==1);stats[stat]++;}
void dm_skill_add_selected(void){CHECK(pumping);skills++;}
void dm_ui_bind_begin(uint64_t key){CHECK(pumping);binds++;last_bind=key;bind_state=1;}
void dm_ui_bind_cancel(void){CHECK(pumping);cancels++;bind_state=0;}
unsigned dm_ui_item_query(float x,float y){CHECK(pumping && x>=0 && y>=0);return ++query_sequence;}
unsigned dm_ui_drop_query(float x,float y,float fy){CHECK(pumping && x>=0 && y>=0 && fy>=0);return ++query_sequence;}
int dm_ui_item_result(unsigned seq){CHECK(pumping && seq==query_sequence);return 0;}
int dm_ui_drop_bag(void){CHECK(pumping);return 0;}
int dm_ui_drop_point(float *x,float *y){CHECK(pumping);(void)x;(void)y;return 0;}
int dm_ui_origin_point(float *x,float *y){CHECK(pumping);(void)x;(void)y;return 0;}
void dm_ui_drag_lift(float y){CHECK(pumping);(void)y;}
void dm_ui_drag_begin(int k,float x,float y,float t){CHECK(pumping);(void)k;(void)x;(void)y;(void)t;}
void dm_ui_drag_move(float x,float y){CHECK(pumping);(void)x;(void)y;}
void dm_ui_drag_end(void){CHECK(pumping);}
void dm_ui_touch(int phase,float x,float y){CHECK(pumping && phase==3);(void)x;(void)y;}
void dm_ui_shop_buy_at(float x,float y){CHECK(pumping);(void)x;(void)y;}
void dm_ui_stack_take_at(float x,float y){CHECK(pumping);(void)x;(void)y;}
void dm_ui_stack_cube_at(float x,float y){CHECK(pumping);(void)x;(void)y;}
void dm_shop_sell_cursor(void){CHECK(pumping);}

static int event(void *p,dm_tc_state *s,uint32_t gen,int slot,int phase,double now){
    (void)p;int defer=integrate_drag?dm_android_d2r_touch_flow_event(s,gen,slot,phase,now):0;
    return defer|dm_android_skill_flow_event(s,gen,slot,phase,now);
}
static dm_android_touch_snapshot snap(void){dm_android_touch_snapshot s;dm_android_touch_snapshot_get(&s);return s;}
static void pump(void){pumping=1;dm_android_skill_flow_pump();if(integrate_drag)dm_android_d2r_touch_flow_pump();pumping=0;}
static void contact(float x,float y){int32_t id=7;CHECK(!dm_android_touch_event(&id,&x,&y,1,0));}
static void lift(float x,float y){CHECK(!dm_android_touch_release(7,x,y));CHECK(!dm_android_touch_event(NULL,NULL,NULL,0,0));}
static void classify(int kind,uint32_t id){
    hit_kind=kind;hit_id=id;dm_android_touch_snapshot s=snap();int slot=s.controls.pointer_slot;
    CHECK(slot>=0);pumping=1;
    dm_android_skill_flow_classified(s.cancel_generation,slot,s.controls.finger[slot].start_time_s);
    pumping=0;
}
static void touch_state(void){
    dm_android_touch_game_state state={.navigation=navigation,.pointer_item=-1,.belt_rows=4};
    dm_android_touch_set_game_state(&state);
}
static void reset(void){
    dm_android_touch_cancel();bind_state=0;busy=unstable=0;native_fresh=1;
    game=1;navigation=1;hit_kind=0;hit_id=focus_id=0;focus_skill=-1;reset_at_read=0;
    points=(dm_points_info){.char_open=1,.stat_points=20,.sel_skill=-1};
    touch_state();pump();
}
static void tap(int kind,uint32_t id){contact(hit_x,hit_y);classify(kind,id);lift(hit_x,hit_y);}
static void set_group(dm_tc_state *s,void *p){s->skill_set=*(int *)p;}
static void skill_target(float *x,float *y,int target){
    dm_android_touch_snapshot s=snap();*x=s.controls.layout.target[target].x/s.controls.layout.width;
    *y=s.controls.layout.target[target].y/s.controls.layout.height;
}

static int bridge_read(void *opaque,dm_android_touch_game_state *state){(void)opaque;(void)state;return 0;}
static void bridge_dispatch(void *opaque,const dm_android_touch_events *events){(void)opaque;(void)events;}
int main(int argc,char **argv){
    if(argc!=2)return 2;g_t0_ns=now_ns();
    dm_android_input_config config={.source="skill-flow-fixture",.panel_width=1280,.panel_height=720,
        .source_ready=ready,.logical_npad_style=1,.logical_npad_id=0};
    CHECK(!dm_android_input_bind(&config) && !dm_android_input_initialize_npad());
    dm_android_touch_bind_game_bridge(bridge_read,bridge_dispatch,NULL);
    dm_android_touch_resize(1280,720,0,0);dm_android_touch_bind_flow(event,NULL);dm_android_touch_set_enabled(1);
    native=(dm_tg_snapshot){.scene_epoch=1,.world_epoch=1,.binding_identity=11,.hud_identity=22,
        .ready=1,.valid=1,.supported=1,.source=DM_TG_SOURCE_NATIVE_GETTER,.identity_scope=DM_TG_IDENTITY_NATIVE};
    uint32_t logical[6]={0x2000,0x1000,0x8000,0x4000,0x200,0x800};
    for(int i=0;i<6;i++)native.mapping[i]=(dm_tg_mapping){logical[i],(uint32_t)i,(uint32_t)i+6};
    reset();

    /* First tap selects only; only the same second target may spend. */
    tap(2,1);CHECK(snap().controls.tap_move_seq==1 && !snap().controls.mouse_click_seq);pump();CHECK(!stats[0]);
    tap(2,1);pump();CHECK(stats[0]==1);
    tap(2,2);pump();CHECK(!stats[1]);tap(2,2);pump();CHECK(stats[1]==1);
    tap(0,0);pump();unsigned spent=stats[1];tap(2,2);pump();CHECK(stats[1]==spent);

    /* Holding starts at 0.5s, repeats once per 0.15s, never catches up after
     * a slow frame and never adds a release tap. */
    reset();spent=stats[2];contact(hit_x,hit_y);classify(2,3);pump();CHECK(stats[2]==spent);
    usleep(530000);pump();CHECK(stats[2]==spent+1 && snap().controls.finger[0].pointer_long);
    pump();CHECK(stats[2]==spent+1);usleep(180000);pump();CHECK(stats[2]==spent+2);
    usleep(350000);pump();CHECK(stats[2]==spent+3);lift(hit_x,hit_y);pump();CHECK(stats[2]==spent+3);
    reset();points.stat_points=0;spent=stats[0];contact(hit_x,hit_y);classify(2,1);usleep(520000);pump();
    CHECK(stats[0]==spent);lift(hit_x,hit_y);pump();CHECK(stats[0]==spent);

    /* The widget id is not a skill number. Old point snapshots/focus cannot
     * pin a different skill, even when no cursor pulse is pending. */
    reset();points.char_open=0;points.tree_open=1;points.skill_points=10;points.sel_skill=99;
    unsigned added=skills;tap(1,0x80000200u);pump();tap(1,0x80000200u);usleep(330000);pump();CHECK(skills==added);
    focus_id=0x80000200u;focus_skill=7;points.sel_skill=7;pump();
    tap(1,0x80000200u);pump();CHECK(skills==added);usleep(330000);pump();CHECK(skills==added+1);
    /* Same opaque widget but changed data/focus cannot receive a stale add. */
    tap(1,0x80000200u);focus_skill=8;points.sel_skill=8;usleep(330000);pump();CHECK(skills==added+1);
    contact(.55f,.35f);classify(0,0);lift(.55f,.35f);pump();CHECK(skills==added+1);
    /* Native block 5 intentionally goes through the game's cap notice. */
    focus_id=0x80000300u;focus_skill=9;points.sel_skill=9;points.sel_block=5;
    tap(1,focus_id);pump();tap(1,focus_id);usleep(330000);pump();CHECK(skills==added+2);
    /* Other block reasons do not enqueue an add. */
    points.sel_block=2;tap(1,focus_id);usleep(330000);pump();CHECK(skills==added+2);

    /* Skill-tree long press emits Y, consumes the release, and owns only its
     * pulse epoch. Existing combat skill-button holds remain continuous casts. */
    reset();points.tree_open=1;points.char_open=0;
    contact(hit_x,hit_y);classify(1,0x80000400u);usleep(530000);pump();
    CHECK(snap().controls.pulse_button==DM_BTN_Y && snap().controls.finger[0].pointer_long);
    unsigned moves=snap().controls.tap_move_seq,clicks=snap().controls.mouse_click_seq;
    lift(hit_x,hit_y);CHECK(snap().controls.tap_move_seq==moves && snap().controls.mouse_click_seq==clicks);
    ui++;pump();CHECK(!snap().controls.pulse_button);

    reset();bind_state=2;points.tree_open=1;pump();
    CHECK(fabsf(snap().controls.pointer_x/1280-.155f)<.001f);
    tap(1,0x80000400u);CHECK(snap().controls.pulse_button==DM_BTN_Y);pump();
    CHECK(skills==added+2);

    /* Frontend/ordinary buttons must still reach the original mouse path. */
    reset();game=0;world++;scene++;pump();clicks=snap().controls.mouse_click_seq;
    tap(0,0);CHECK(snap().controls.mouse_click_seq==clicks+1);pump();
    reset();clicks=snap().controls.mouse_click_seq;tap(0,0);CHECK(snap().controls.mouse_click_seq==clicks+1);

    /* UI/world/input and native HUD identity changes cancel live gestures. */
    for(int why=0;why<8;why++) {
        reset();contact(hit_x,hit_y);classify(2,1);spent=stats[0];
        if(why==0)ui++;else if(why==1)scene++;else if(why==2)world++;
        else if(why==3)native.binding_identity++;else if(why==4)native.hud_identity++;
        else if(why==5)native.selected_bank=native.selected_bank?0:16;
        else if(why==6)dm_android_input_cancel();else dm_android_touch_cancel();
        pump();CHECK(snap().controls.pointer_slot<0);lift(hit_x,hit_y);pump();CHECK(stats[0]==spent);
    }
    /* A delayed answer for a reused slot cannot classify the new contact. */
    reset();contact(hit_x,hit_y);dm_android_touch_snapshot old=snap();
    double old_start=old.controls.finger[old.controls.pointer_slot].start_time_s;
    lift(hit_x,hit_y);usleep(1000);contact(hit_x,hit_y);
    hit_kind=2;hit_id=1;pumping=1;dm_android_skill_flow_classified(old.cancel_generation,old.controls.pointer_slot,old_start);pumping=0;
    moves=snap().controls.tap_move_seq;lift(hit_x,hit_y);CHECK(snap().controls.tap_move_seq==moves);
    /* A lifecycle reset during final action validation invalidates dispatch. */
    reset();tap(2,1);pump();tap(2,1);spent=stats[0];reset_at_read=point_reads+2;pump();CHECK(stats[0]==spent);

    /* Main attack and both normal/alternate groups use the real empty glyph
     * contract. Move-out, stale mapping, and a newly bound slot do not bind. */
    for(int group=0;group<2;group++)for(int target=DM_TC_MAIN;target<=DM_TC_SKILL4;target++) {
        reset();navigation=0;scene++;touch_state();
        for(int i=0;i<12;i++)native.raw_skill_id[i]=UINT32_MAX;
        dm_android_touch_snapshot s=snap();dm_android_touch_edit(s.cancel_generation,set_group,&group);pump();
        float x,y;skill_target(&x,&y,target);unsigned bound=binds;
        contact(x,y);s=snap();CHECK(s.controls.finger[0].target==DM_TC_EDGE && !s.controls.buttons);
        lift(x,y);pump();if(binds!=bound+1)LOG("empty bind group=%d target=%d before=%u after=%u bind_state=%d gen=%u",group,target,bound,binds,bind_state,snap().cancel_generation);CHECK(binds==bound+1);
        CHECK(last_bind==(s.controls.layout.target[target].button|(group?DM_BTN_ZL:0)));
        /* Opening our expected skill tree may rebuild UI/HUD without aborting
         * ownership. A lifecycle/input reset must cancel that owned flow. */
        unsigned cancelled=cancels;navigation=1;scene++;ui++;native.hud_identity++;touch_state();bind_state=2;pump();CHECK(cancels==cancelled);
        dm_android_input_cancel();pump();CHECK(cancels==cancelled+1);
    }
    reset();navigation=0;scene++;touch_state();pump();float x,y;skill_target(&x,&y,DM_TC_MAIN);
    unsigned bound=binds;contact(x,y);contact(x-.1f,y);lift(x-.1f,y);pump();CHECK(binds==bound);
    reset();navigation=0;scene++;touch_state();pump();skill_target(&x,&y,DM_TC_MAIN);contact(x,y);
    for(int i=0;i<12;i++)native.raw_skill_id[i]=0;lift(x,y);pump();CHECK(binds==bound);
    reset();navigation=0;scene++;touch_state();native_fresh=0;native.captured_ns=now_ns()-300000000ull;pump();
    skill_target(&x,&y,DM_TC_MAIN);contact(x,y);CHECK(snap().controls.finger[0].target==DM_TC_MAIN);lift(x,y);pump();CHECK(binds==bound);
    /* A bound combat skill hold does not enter the rebind flow. */
    reset();navigation=0;scene++;touch_state();for(int i=0;i<12;i++)native.raw_skill_id[i]=0;pump();
    skill_target(&x,&y,DM_TC_SKILL1);contact(x,y);usleep(530000);pump();CHECK(snap().controls.finger[0].target==DM_TC_SKILL1);
    lift(x,y);pump();CHECK(binds==bound);

    /* Finally run both production flow modules together. A quick physical
     * release waits for its own origin reply before select/spend rewriting. */
    integrate_drag=1;reset();hit_kind=2;hit_id=1;spent=stats[0];
    moves=snap().controls.tap_move_seq;clicks=snap().controls.mouse_click_seq;
    contact(hit_x,hit_y);lift(hit_x,hit_y);CHECK(snap().controls.pointer_slot>=0);
    pump();pump();CHECK(snap().controls.pointer_slot<0 && snap().controls.tap_move_seq==moves+1);
    pump();CHECK(stats[0]==spent && snap().controls.mouse_click_seq==clicks);
    contact(hit_x,hit_y);lift(hit_x,hit_y);pump();pump();pump();CHECK(stats[0]==spent+1);
    reset();bind_state=2;points.tree_open=1;hit_kind=1;hit_id=0x80000400u;pump();
    contact(hit_x,hit_y);lift(hit_x,hit_y);pump();pump();
    CHECK(snap().controls.pulse_button==DM_BTN_Y && snap().controls.pointer_slot<0);

    FILE *out=fopen(argv[1],"w");CHECK(out);
    fprintf(out,"{\"passed\":true,\"checks\":%u,\"fixture_only\":true,\"production_transport_controls_skill_flow\":true,\"generated_native_resolver\":true,\"select_then_spend\":true,\"stat_repeat\":true,\"skill_long_press_bind\":true,\"normal_alt_main_empty_slots\":true,\"focus_identity_gate\":true,\"generation_aba_cancel\":true,\"frontend_mouse_passthrough\":true,\"combined_drag_classify_release\":true,\"host_calls_only_in_game_pump\":true,\"original_game_ui_executed\":false,\"app_save_data_used\":false}\n",checks);
    CHECK(!fclose(out));return 0;
}
