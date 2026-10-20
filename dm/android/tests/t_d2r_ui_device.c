/* The shipped app library is exercised with real shared UI/input code and a
 * synthetic widget tree. No title resources, running game or save is opened. */
#include "../../host/dm.h"
#include "../../host/hle.h"
#include "../../host/dm_cursor.h"
#include "../../host/dm_input.h"
#include "../input_backend.h"
#include "../touch_controls_android.h"
#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdatomic.h>

static int checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"UI fixture failed line=%d\n",__LINE__);return 1; } } while(0)
#define SYM(type,name) type name=(type)dlsym(lib,#name);assert(name)
static void put64(void *p,size_t off,uint64_t v) {memcpy((uint8_t *)p+off,&v,8);}
static void widget(uint8_t *p,const char *name) {memset(p,0,0x200);put64(p,8,(uintptr_t)name);p[0x50]=p[0x51]=1;}
static int source_ready=1;
static int ready(void *p) {(void)p;return source_ready;}
static struct { uint64_t category,action;int32_t x,y,arg; } messages[32];
static unsigned message_count;
static uint64_t name_hash(const char *s) {uint64_t h=0xcbf29ce484222325ull;for(;*s;s++)h=(h^(uint8_t)*s)*0x100000001b3ull;return h;}
/* Observe the actual widget virtual-call boundary. This fixture handler does
 * not claim to exercise the original game's implementation of that method. */
static void message_record(uint64_t root,void *message) {
    (void)root;assert(message_count<32);uint64_t *msg=message;int32_t *payload=(void *)msg[2];
    messages[message_count].category=msg[0];messages[message_count].action=msg[1];
    messages[message_count].x=payload[1];messages[message_count].y=payload[2];
    messages[message_count].arg=payload[4];message_count++;
}
static int slider_hit(uint64_t widget,uint64_t xy) {
    (void)widget;int x=(int32_t)xy,y=(int32_t)(xy>>32);
    return x>=100 && x<=900 && y>=200 && y<=300;
}
int main(int argc,char **argv) {
    assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib) {fprintf(stderr,"dlopen %s\n",dlerror());return 1;}
    module_t *mods=dlsym(lib,"g_mod");int *nmods=dlsym(lib,"g_nmod");assert(mods && nmods);
    int (*attach)(module_t *)=dlsym(lib,"dm_android_d2r_ui_attach");
    void (*gate)(uint64_t,const char *,int,uint64_t)=dlsym(lib,"gate_register");
    int (*shown)(const char *)=dlsym(lib,"dm_ui_panel_shown");
    uint32_t (*gen)(void)=dlsym(lib,"dm_ui_gen");
    uint64_t (*ref_get)(dm_wref *)=dlsym(lib,"dm_wref_get");
    void (*frame)(void)=dlsym(lib,"dm_guest_frame_tick");
    void (*events)(void *,const dm_android_touch_events *)=dlsym(lib,"dm_android_d2r_ui_events");
    int (*players)(void)=dlsym(lib,"dm_hud_requested_players");
    int (*active)(void)=dlsym(lib,"dm_ui_touch_active");
    int (*frontend_touch)(int,float,float)=dlsym(lib,"dm_android_d2r_frontend_touch");
    void (*touch)(int,float,float)=dlsym(lib,"dm_ui_touch");
    int (*bind)(const dm_android_input_config *)=dlsym(lib,"dm_android_input_bind");
    int (*init)(void)=dlsym(lib,"dm_android_input_initialize_npad");
    int (*publish)(uint64_t,int32_t,int32_t,int32_t,int32_t)=dlsym(lib,"dm_android_input_publish_actions");
    int (*sample)(dm_android_npad_state *,uint32_t,uint32_t)=dlsym(lib,"dm_android_input_read_npad");
    void (*right)(int32_t *,int32_t *)=dlsym(lib,"dm_input_sampled_right");
    void (*pulse)(uint64_t,int)=dlsym(lib,"dm_ui_key_pulse");
    void (*resize)(float,float,float,float)=dlsym(lib,"dm_android_touch_resize");
    void (*enable)(int)=dlsym(lib,"dm_android_touch_set_enabled");
    void (*set_state)(const dm_android_touch_game_state *)=dlsym(lib,"dm_android_touch_set_game_state");
    int (*contact)(const int32_t *,const float *,const float *,int,int)=dlsym(lib,"dm_android_touch_event");
    void (*get_events)(dm_android_touch_events *)=dlsym(lib,"dm_android_touch_get_events");
    void (*consume)(const dm_android_touch_events *)=dlsym(lib,"dm_android_touch_consume_events");
    void (*cancel)(void)=dlsym(lib,"dm_android_input_cancel");
    void (*alone)(uint64_t,int)=dlsym(lib,"dm_ui_key_pulse_alone");
    void (*ingame)(int)=dlsym(lib,"dm_uiscreen_set_ingame");
    void (*mouse_pump)(void)=dlsym(lib,"dm_ui_mouse_pump");
    void (*cursor_clear)(void)=dlsym(lib,"dm_cursor_clear");
    void (*drag_begin)(int,float,float,float)=dlsym(lib,"dm_ui_drag_begin");
    void (*drag_move)(float,float)=dlsym(lib,"dm_ui_drag_move");
    void (*drag_end)(void)=dlsym(lib,"dm_ui_drag_end");
    assert(attach&&gate&&shown&&gen&&ref_get&&frame&&events&&players&&active&&touch&&bind&&init&&publish&&sample&&right&&pulse&&resize&&enable&&set_state&&contact&&get_events&&consume&&cancel&&alone&&ingame&&mouse_pump&&cursor_clear);
    CHECK(frontend_touch && !frontend_touch(0,.5f,.5f));
    *nmods=1;strcpy(mods[0].name,"main");mods[0].size=0x3220000;mods[0].base=calloc(1,mods[0].size);assert(mods[0].base);
    CHECK(!attach(&mods[0]));strcpy(mods[0].build_id,"db0507f36b84200ac05f75d76ed5026f");
    CHECK(!attach(&mods[0]));gate(0x1000,"obs",OBS_UIMSG,0);gate(0x2000,"obs",OBS_TICK,0);gate(0x3000,"obs",OBS_C2S,0);
    CHECK(!attach(&mods[0]));gate(0x4000,"svc",SVC_HOST_C2S_DELIVER,0);CHECK(attach(&mods[0]));
    CHECK(!frontend_touch(-1,.5f,.5f) && !frontend_touch(4,.5f,.5f) && !frontend_touch(0,NAN,.5f) && !active());
    CHECK(frontend_touch(0,.25f,.75f) && active());
    CHECK(frontend_touch(1,.3f,.7f) && frontend_touch(2,.3f,.7f));
    CHECK(frontend_touch(3,0,0) && !active());
    ingame(1);CHECK(!frontend_touch(0,.5f,.5f) && !active());
    touch(0,.5f,.5f);CHECK(frontend_touch(3,0,0) && !active());ingame(0);

    uint8_t root[0x200],panel[0x200],leaf[0x200],nested[0x200];uint64_t children[2]={(uintptr_t)panel,(uintptr_t)nested},leaves[1]={(uintptr_t)leaf};
    widget(root,"root");widget(panel,"BankExpansionLayout");widget(leaf,"leaf");widget(nested,"other");
    put64(root,0x58,(uintptr_t)children);put64(root,0x60,2);put64(panel,0x58,(uintptr_t)leaves);put64(panel,0x60,1);
    put64(mods[0].base,0x2f05458,(uintptr_t)root);
    CHECK(shown("BankExpansionLayout"));CHECK(!shown("NpcTradePanel"));
    panel[0x50]=0;CHECK(!shown("BankExpansionLayout"));panel[0x50]=1;
    leaf[0x51]=0;CHECK(!shown("BankExpansionLayout"));leaf[0x51]=1;CHECK(shown("BankExpansionLayout"));
    put64(root,0x60,257);CHECK(!shown("BankExpansionLayout"));put64(root,0x60,2);
    uint32_t generation=gen();dm_wref ref={(uintptr_t)panel,generation};CHECK(ref_get(&ref)==(uintptr_t)panel);
    children[0]=0;frame();CHECK(gen()>generation && !ref_get(&ref));
    children[0]=(uintptr_t)panel;frame();CHECK(ref_get(&ref)==(uintptr_t)panel);
    put64(mods[0].base,0x2f05458,0);frame();

    dm_android_touch_events event={.diff_request=7,.mouse_click_seq=1,.mouse_x=.25f,.mouse_y=.75f};
    events(NULL,&event);CHECK(players()==7 && active());touch(3,0,0);CHECK(!active());
    uint64_t vtable[16]={0};vtable[5]=(uintptr_t)message_record;widget(root,"root");put64(root,0,(uintptr_t)vtable);
    put64(mods[0].base,0x2f05458,(uintptr_t)root);
    int32_t dimensions[2]={1280,720};memcpy(mods[0].base+0x211da38,dimensions,sizeof dimensions);
    events(NULL,&event);
    for(int i=0;i<4;i++) {frame();mouse_pump();unsigned count=message_count;mouse_pump();CHECK(message_count==count);}
    CHECK(!active() && message_count==6);
    for(unsigned i=0;i<message_count;i++)CHECK(messages[i].category==name_hash("InputMessage") && abs(messages[i].x-320)<=1 && abs(messages[i].y-540)<=1);
    CHECK(messages[0].action==name_hash("MouseMove") && messages[1].action==name_hash("MouseMove") && messages[2].action==name_hash("MouseMove"));
    CHECK(messages[3].action==name_hash("LeftMouseButtonDown") && messages[4].action==name_hash("MouseMove") && messages[5].action==name_hash("LeftMouseButtonUp"));
    /* Exercise the actual host gesture consumer as well as the separate
     * Android flow fixture: scroll goes through widget InputMessage, and a
     * slider uses its own virtual hit-test plus paired down/up messages. */
    CHECK(drag_begin && drag_move && drag_end);
    unsigned count=message_count;drag_begin(1,.25f,.3f,.05f);drag_move(.25f,.6f);frame();mouse_pump();
    CHECK(message_count==count+2 && messages[count].action==name_hash("MouseMove") &&
          messages[count+1].action==name_hash("MouseScroll") && messages[count+1].arg==360);
    drag_end();count=message_count;frame();mouse_pump();CHECK(message_count==count);
    uint8_t slider[0x200];widget(slider,"fixture-slider");
    uint64_t *slider_vt=(uint64_t *)(mods[0].base+0x20bad70);slider_vt[9]=(uintptr_t)slider_hit;
    put64(slider,0,(uintptr_t)slider_vt);uint64_t sliders[1]={(uintptr_t)slider};
    put64(root,0x58,(uintptr_t)sliders);put64(root,0x60,1);
    drag_begin(2,.3f,.35f,.05f);drag_move(.7f,.35f);frame();mouse_pump();
    CHECK(message_count==count+2 && messages[count+1].action==name_hash("LeftMouseButtonDown") &&
          messages[count+1].x>=890 && messages[count+1].x<=900 && abs(messages[count+1].y-250)<=1);
    drag_end();frame();mouse_pump();CHECK(message_count==count+4 && messages[count+3].action==name_hash("LeftMouseButtonUp"));
    frame();mouse_pump();CHECK(message_count==count+4);
    put64(mods[0].base,0x2f05458,0);frame();cursor_clear();
    dm_android_input_config config={.source="isolated-ui-fixture",.source_ready=ready,.panel_width=1280,.panel_height=720,.logical_npad_style=1,.logical_npad_id=0};
    CHECK(!bind(&config)&&!init());CHECK(!publish(DM_BTN_B,1000,-2000,3000,-4000));pulse(DM_BTN_L,10);
    dm_android_npad_state state;CHECK(!sample(&state,0,1));
    CHECK(state.buttons==(DM_BTN_B|DM_BTN_L) && state.lx==1000 && state.ly==-2000 && state.rx==3000 && state.ry==-4000);
    int32_t rx,ry;right(&rx,&ry);CHECK(rx==state.rx && ry==state.ry);
    _Atomic int *presses=dlsym(lib,"g_dm_l_presses");CHECK(presses && atomic_load(presses)==1);
    CHECK(!sample(&state,0,1) && atomic_load(presses)==1);
    pulse(0,1);CHECK(!publish(0,0,0,0,0)&&!sample(&state,0,1));pulse(DM_BTN_L,10);
    CHECK(!sample(&state,0,1) && atomic_load(presses)==2);

    /* A held L before cancellation must not hide the first post-reset edge. */
    pulse(0,1);cancel();CHECK(!publish(DM_BTN_L,0,0,0,0));
    CHECK(!sample(&state,0,1) && atomic_load(presses)==3);
    source_ready=0;CHECK(!sample(&state,0,1) && !state.attributes);
    source_ready=1;CHECK(!publish(DM_BTN_L,0,0,0,0));
    CHECK(!sample(&state,0,1) && atomic_load(presses)==4);
    cancel();CHECK(!sample(&state,0,1));
    ingame(1);alone(DM_BTN_A,4);CHECK(!sample(&state,0,1) && state.buttons==DM_BTN_A);
    CHECK(!publish(DM_BTN_B,0,0,0,0) && !sample(&state,0,1) && state.buttons==DM_BTN_A);
    cancel();for(int i=0;i<5;i++)frame();
    CHECK(!sample(&state,0,1) && !state.buttons);ingame(0);

    dm_android_touch_game_state game={.belt_rows=4,.pointer_item=-1};set_state(&game);resize(1280,720,0,0);enable(1);
    int32_t id=3;float x=.45f,y=.3f;CHECK(!contact(&id,&x,&y,1,0)&&!contact(NULL,NULL,NULL,0,0));
    get_events(&event);CHECK(event.world_tap_seq && fabsf(event.world_x-x)<.00001f && fabsf(event.world_y-y)<.00001f);
    consume(&event);get_events(&event);CHECK(!event.world_tap_seq);
    printf("ANDROID_D2R_UI_OK checks=%d version_gates=4 real_widget_tree=1 stale_widget_revalidation=1 event_queues=2 npad_final_filter=1 normalized_coordinates=1 reset_edges=1 reset_deferred_keys=1 widget_message_delivery=1 hover_down_up=1 scroll_slider_messages=1\n",checks);
    return 0;
}
