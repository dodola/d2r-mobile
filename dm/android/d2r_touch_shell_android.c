#include "d2r_touch_shell_android.h"
#include "d2r_host_modules.h"
#include "input_backend.h"
#include "../host/dm.h"
#include "../host/dm_cursor.h"
#include "../host/dm_world.h"
#include <math.h>
#include <string.h>

extern int dm_padmode_busy(void);
typedef struct {uint32_t generation,ui;uint64_t input,scene,world;} shell_scope;
typedef struct {
    int valid,game,navigation,pad,free_cursor,full,have_mini,bind,touch,pending;
    float mini[4];uint64_t captured;shell_scope scope;
} shell_observation;
typedef struct {int active,map,dragged;double start;float x,y;uint32_t move,mouse,item,use,ident,stack,long_press;} shell_contact;
static struct {
    shell_observation cache;
    shell_contact contact[DM_TC_MAX_TOUCH];
    uint64_t revision;
    double tip_until;
    int panel_seen,parked,map_request,map_wait;
    float map_x,map_y;unsigned world_seq;shell_scope map_scope;
} shell;
static int equal(shell_scope a,shell_scope b){return a.generation==b.generation&&a.ui==b.ui&&a.input==b.input&&a.scene==b.scene&&a.world==b.world;}
static int recent(const shell_observation *o,double now){uint64_t ns=(uint64_t)(now*1e9);return o->valid&&ns>=o->captured&&ns-o->captured<=250000000ULL;}

void dm_android_d2r_touch_shell_event(dm_tc_state *s,uint32_t generation,int slot,int phase,double now){
    if(phase==DM_ATF_RESET){++shell.revision;memset(shell.contact,0,sizeof shell.contact);shell.cache.valid=0;shell.map_request=shell.map_wait=0;shell.panel_seen=shell.parked=0;shell.tip_until=0;return;}
    if(slot<0||slot>=DM_TC_MAX_TOUCH)return;
    shell_contact *c=&shell.contact[slot];dm_tc_finger *f=&s->finger[slot];
    if(phase==DM_ATF_BEGIN){
        ++shell.revision;shell.map_request=shell.map_wait=0;
        *c=(shell_contact){.active=1,.start=now,.x=f->aim_origin_x,.y=f->aim_origin_y};
        if(s->navigation_mode&&s->pointer_slot==slot){shell.tip_until=-1;shell.parked=0;}
        shell_observation *o=&shell.cache;
        if(s->navigation_mode||!recent(o,now)||!o->game||o->navigation||o->pad||o->scope.generation!=generation||s->layout.width<=0||s->layout.height<=0)return;
        int hit=dm_tc_hit(&s->layout,f->x,f->y);
        /* 隐藏的地图键不占小地图点击区；技能拖动仍沿用其取消区域。
         * The hidden map key yields to minimap taps; skill drags keep their cancellation region. */
        if(hit==DM_TC_MAP && !(s->enabled_targets&(1u<<DM_TC_MAP)))hit=DM_TC_NONE;
        if(hit>=0||hit==DM_TC_SWAP||f->target==DM_TC_DIFF||f->target==DM_TC_EDGE)return;
        float band=(o->mini[3]-o->mini[1])*s->layout.height/4;
        if(band<30)band=fminf(30,(o->mini[3]-o->mini[1])*s->layout.height*.4f);
        int strip=o->have_mini&&f->x>=o->mini[0]*s->layout.width&&f->x<=o->mini[2]*s->layout.width&&f->y>=o->mini[1]*s->layout.height&&f->y<=o->mini[1]*s->layout.height+band;
        if(strip||(o->full&&f->target==DM_TC_NONE)){
            dm_tc_finger saved=*f;dm_tc_cancel_finger(s,slot);saved.target=DM_TC_EDGE;saved.key_down=0;saved.aim_cancelled=1;s->finger[slot]=saved;
            c->map=o->full?2:1;
        }
    }
    if(!c->active)return;
    if(phase==DM_ATF_END){
        if(c->map){
            if(!s->navigation_mode&&recent(&shell.cache,now)&&shell.cache.scope.generation==generation&&now-c->start<.6&&hypotf(f->x-c->x,f->y-c->y)<=14){
                shell.map_request=c->map;shell.map_x=f->x/s->layout.width;shell.map_y=f->y/s->layout.height;shell.map_scope=shell.cache.scope;
            }
            return;
        }
        if(s->navigation_mode&&s->pointer_slot==slot){
            c->dragged=f->pointer_dragged||f->shop_drag||f->ui_drag;
            c->move=s->tap_move_seq;c->mouse=s->mouse_click_seq;c->item=s->item_tap_seq;c->use=s->use_seq;c->ident=s->ident_seq;c->stack=s->stack_take_seq;c->long_press=s->long_press_seq;
            c->map=-1;
        }
    }
    if(phase==DM_ATF_AFTER_END){
        if(c->map==-1&&s->navigation_mode){
            int item=s->item_tap_seq!=c->item&&!c->dragged&&s->use_seq==c->use&&s->ident_seq==c->ident&&s->stack_take_seq==c->stack&&s->long_press_seq==c->long_press&&s->mouse_click_seq==c->mouse;
            shell.tip_until=s->tap_move_seq!=c->move||item?-1:now+(s->mouse_click_seq!=c->mouse?.05:2.8);
        }
        c->active=0;
    }
}
typedef struct {shell_observation observation;uint64_t revision;int request,wait;unsigned seq;float x,y;int park,clear;dm_tc_state controls;} shell_edit;
static void shell_copy(dm_tc_state *s,void *opaque){shell_edit *e=opaque;e->revision=shell.revision;e->observation=shell.cache;e->controls=*s;e->request=shell.map_request;e->wait=shell.map_wait;e->seq=shell.world_seq;e->x=shell.map_x;e->y=shell.map_y;}
static void shell_publish(dm_tc_state *s,void *opaque){
    shell_edit *e=opaque;if(shell.revision!=e->revision)return;
    shell_observation o=e->observation;
    if(!shell.cache.valid||!equal(shell.cache.scope,o.scope)){shell.panel_seen=0;shell.tip_until=0;shell.map_request=shell.map_wait=0;if(shell.parked)s->pointer_valid=0;shell.parked=0;}
    shell.cache=o;
    if(!o.valid||!o.game||o.pad||!s->navigation_mode){if(shell.parked)s->pointer_valid=0;shell.parked=0;shell.panel_seen=0;shell.tip_until=0;}
    else if(s->pointer_slot>=0)shell.tip_until=-1;
    else if(!shell.panel_seen){if(o.free_cursor){shell.tip_until=o.captured/1e9+.3;shell.panel_seen=1;}}
    else if(shell.tip_until>0&&o.captured/1e9>=shell.tip_until&&o.free_cursor&&!s->holding&&!o.bind&&!o.touch&&!o.pending&&!s->pulse_button&&!dm_tc_pulse_pending(s)){
        s->pointer_valid=1;s->pointer_x=.02f*s->layout.width;s->pointer_y=.5f*s->layout.height;shell.parked=1;shell.tip_until=0;
    }
}
static void shell_map_begin(dm_tc_state *s,void *opaque){
    (void)s;shell_edit *e=opaque;
    if(shell.revision!=e->revision||!shell.cache.valid||!equal(shell.map_scope,e->observation.scope))return;
    e->request=shell.map_request;shell.map_request=0;shell.map_wait=e->request==1;e->wait=shell.map_wait;
}
static void shell_map_seq(dm_tc_state *s,void *opaque){(void)s;shell_edit *e=opaque;if(shell.revision==e->revision&&shell.map_wait)shell.world_seq=e->seq;}
static void shell_map_finish(dm_tc_state *s,void *opaque){(void)s;shell_edit *e=opaque;e->request=0;if(shell.revision==e->revision&&shell.map_wait&&shell.world_seq==e->seq&&equal(shell.map_scope,e->observation.scope)){shell.map_wait=0;e->request=1;}}
void dm_android_d2r_touch_shell_pump(void){
    dm_android_touch_snapshot snapshot;dm_android_touch_snapshot_get(&snapshot);
    shell_edit e={0};if(!dm_android_touch_edit(snapshot.cancel_generation,shell_copy,&e))return;
    dm_android_input_status input;dm_android_input_status_get(&input);dm_android_d2r_scene_status scene;dm_android_d2r_scene_status_get(&scene);
    shell_observation o={.valid=snapshot.ready&&snapshot.enabled&&input.source_ready&&scene.attached,.game=scene.game,.navigation=scene.navigation,
        .pad=input.controller_mode||dm_padmode_busy(),.captured=now_ns(),.scope={snapshot.cancel_generation,dm_ui_gen(),input.input_resets,scene.scene_epoch,scene.world_epoch}};
    if(o.valid&&!o.pad){o.free_cursor=dm_ui_free_cursor();o.full=dm_ui_map_is_full();o.have_mini=dm_ui_minimap_rect(o.mini);o.bind=dm_ui_bind_active();o.touch=dm_ui_touch_active();o.pending=dm_cursor_pending();}
    e.observation=o;dm_android_touch_edit(snapshot.cancel_generation,shell_publish,&e);
    if(!o.valid||!o.game||o.navigation||o.pad)return;
    /* A map tap first asks the real world selector. No target means full map;
     * target classes 1..5 already interact through the original world pump. */
    if(e.request){e.request=0;dm_android_touch_edit(snapshot.cancel_generation,shell_map_begin,&e);
        if(e.request==2){if(o.full)dm_ui_map_full(0);}
        else if(e.request==1){e.seq=dm_world_tap(e.x,e.y);dm_android_touch_edit(snapshot.cancel_generation,shell_map_seq,&e);}
    }else if(e.wait){int result=dm_world_tap_result(e.seq);if(result>=0){dm_android_touch_edit(snapshot.cancel_generation,shell_map_finish,&e);if(e.request&&result==0)dm_ui_map_full(1);}}
}
