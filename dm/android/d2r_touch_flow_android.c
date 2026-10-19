/* Android counterpart of DmTouchOverlay's asynchronous item/drop handshake.
 * Portable gestures are shared verbatim with iOS; Java publishes coordinates,
 * while every title query and queued action below runs at the guest HID poll. */
#include "d2r_touch_flow_android.h"
#include "d2r_host_modules.h"
#include "input_backend.h"
#include "../host/dm.h"
#include "../host/dm_cursor.h"
#include "../host/dm_actions.h"
#include <math.h>
#include <string.h>
#include <stdatomic.h>

extern int dm_padmode_busy(void);
extern uint64_t dm_guest_frame(void);
extern void dm_android_skill_flow_classified(uint32_t,int,double) __attribute__((weak));
extern void dm_android_d2r_touch_shell_event(dm_tc_state *,uint32_t,int,int,double) __attribute__((weak));
extern void dm_vk_ui_ghost_request(int,float,float,const float *,float,float,int,const _Atomic uint64_t *,uint64_t) __attribute__((weak));

typedef struct {
    uint32_t ui;
    uint64_t input,world;
} flow_scope;
typedef struct {
    uint64_t serial;
    uint32_t generation;
    int active,slot,kind,need_origin,origin_pending;
    unsigned origin_seq;
    int need_drop,drop_pending,drop_final,last_drop,last_drop_final,last_bag,last_point;
    unsigned drop_seq;
    float drop_x,drop_y,origin_x,origin_y;
    int end_pending,dragged,host_drag,want_drag,cleanup;
    uint64_t host_drag_frame;
    float host_x,host_y;
    double end_time,end_wall;
    float width,height,ox,oy,x,y;
    flow_scope scope;
    int have_scope;
} touch_flow;
/* Protected by touch_controls_android's state lock, including these event
 * hooks and all edit callbacks. Never retained guest pointers. */
static touch_flow flow={.slot=-1,.kind=-1,.last_drop=-1};
/* A physical UP/cancel invalidates render work immediately, even while an
 * asynchronous final drop answer still keeps the portable contact alive. */
static _Atomic uint64_t ghost_live_contact;

static void clear_flow(uint32_t generation) {
    atomic_store_explicit(&ghost_live_contact,0,memory_order_release);
    uint64_t serial=flow.serial+1;int cleanup=flow.cleanup || flow.host_drag;
    flow=(touch_flow){.serial=serial,.generation=generation,.slot=-1,.kind=-1,
        .last_drop=-1,.cleanup=cleanup};
}
static int pointer_is(const dm_tc_state *s,int slot) {
    return s->navigation_mode && slot>=0 && slot<DM_TC_MAX_TOUCH &&
        s->pointer_slot==slot && s->finger[slot].active;
}
static void track_move(dm_tc_state *s) {
    if(!flow.active || !pointer_is(s,flow.slot))return;
    dm_tc_finger *f=&s->finger[flow.slot];
    flow.x=f->x;flow.y=f->y;
    flow.want_drag=f->ui_drag;
    flow.dragged=f->pointer_dragged || f->shop_drag;
    if(flow.dragged)flow.need_drop=1;
}
int dm_android_d2r_touch_flow_event(dm_tc_state *s,uint32_t generation,int slot,int phase,double now) {
    if(dm_android_d2r_touch_shell_event)dm_android_d2r_touch_shell_event(s,generation,slot,phase,now);
    if(phase==DM_ATF_RESET) {clear_flow(generation);return 0;}
    if(phase==DM_ATF_BEGIN && pointer_is(s,slot)) {
        clear_flow(generation);flow.active=1;flow.slot=slot;flow.need_origin=1;
        atomic_store_explicit(&ghost_live_contact,flow.serial,memory_order_release);
        flow.width=s->layout.width;flow.height=s->layout.height;
        flow.ox=flow.x=s->finger[slot].aim_origin_x;
        flow.oy=flow.y=s->finger[slot].aim_origin_y;
        /* A move may arrive before the guest has classified the origin.
         * Class 6 suppresses accidental scrolling/pickup until that answer. */
        dm_tc_pointer_item(s,6);return 0;
    }
    if(!flow.active || generation!=flow.generation || slot!=flow.slot)return 0;
    if(phase==DM_ATF_MOVE)track_move(s);
    if(phase==DM_ATF_RELEASE && pointer_is(s,slot)) {
        atomic_store_explicit(&ghost_live_contact,0,memory_order_release);
        track_move(s);
        if(flow.need_origin || flow.origin_pending || flow.dragged || flow.want_drag) {
            flow.end_pending=1;flow.end_time=now;flow.end_wall=now;
            if(flow.dragged)flow.need_drop=1;
            return 1;
        }
    }
    if(phase==DM_ATF_AFTER_END) {
        atomic_store_explicit(&ghost_live_contact,0,memory_order_release);
        flow.active=0;flow.end_pending=0;
        flow.cleanup|=flow.host_drag;flow.want_drag=0;
    }
    return 0;
}

typedef struct {touch_flow flow;dm_tc_state controls;} capture;
static void capture_edit(dm_tc_state *s,void *opaque) {
    capture *c=opaque;c->flow=flow;c->controls=*s;
}
static int capture_now(capture *c,uint32_t generation) {
    return dm_android_touch_edit(generation,capture_edit,c);
}
static int same(const touch_flow *f) {
    return flow.serial==f->serial && flow.generation==f->generation && flow.active;
}
typedef struct {touch_flow expected;flow_scope scope;int result,bag,point,origin,classified,accepted;
    float x,y,origin_x,origin_y;unsigned seq;uint64_t frame;double now,contact_start;int mode;} reply;
enum { REPLY_SCOPE,REPLY_ORIGIN_QUERY,REPLY_ORIGIN,REPLY_DROP_QUERY,REPLY_DROP,REPLY_DRAG,REPLY_CLEAN,REPLY_ABORT };
static void reply_edit(dm_tc_state *s,void *opaque) {
    reply *r=opaque;
    if(r->mode==REPLY_CLEAN) {if(flow.serial==r->expected.serial){flow.cleanup=0;flow.host_drag=0;}return;}
    if(r->mode==REPLY_ABORT) {if(same(&r->expected))clear_flow(flow.generation);return;}
    if(!same(&r->expected) || !pointer_is(s,flow.slot))return;
    r->accepted=1;
    switch(r->mode) {
    case REPLY_SCOPE:flow.scope=r->scope;flow.have_scope=1;break;
    case REPLY_ORIGIN_QUERY:flow.need_origin=0;flow.origin_pending=1;flow.origin_seq=r->seq;break;
    case REPLY_ORIGIN:
        flow.origin_pending=0;flow.kind=r->result;
        dm_tc_pointer_item(s,r->result);
        /* Re-evaluate a move which arrived while origin classification was
         * pending, preserving the physical release timestamp when deferred. */
        dm_tc_move_at(s,flow.slot,flow.x,flow.y,flow.end_pending?flow.end_time:r->now);
        dm_tc_tick(s,flow.end_pending?flow.end_time:r->now);
        track_move(s);r->classified=1;r->contact_start=s->finger[flow.slot].start_time_s;break;
    case REPLY_DROP_QUERY: {
        /* RELEASE/MOVE may arrive while the game thread publishes its query.
         * The answer belongs to the endpoint captured before that call. */
        int endpoint=r->expected.x==flow.x && r->expected.y==flow.y;
        flow.need_drop=!endpoint || (flow.end_pending && !r->expected.end_pending);
        flow.drop_pending=1;flow.drop_seq=r->seq;
        flow.drop_final=endpoint && r->expected.end_pending && flow.end_pending;
        flow.last_drop_final=0;break;
    }
    case REPLY_DROP:
        flow.drop_pending=0;flow.last_drop=r->result;flow.last_bag=r->bag;
        flow.last_drop_final=flow.drop_final;
        flow.last_point=r->point;flow.drop_x=r->x;flow.drop_y=r->y;
        if(r->origin){flow.origin_x=r->origin_x;flow.origin_y=r->origin_y;}
        break;
    case REPLY_DRAG:
        if(!flow.host_drag || flow.host_x!=flow.x || flow.host_y!=flow.y)
            flow.host_drag_frame=r->frame;
        flow.host_drag=flow.want_drag;flow.host_x=flow.x;flow.host_y=flow.y;break;
    default:break;
    }
}
static int apply(reply *r) {
    r->accepted=0;
    return dm_android_touch_edit(r->expected.generation,reply_edit,r) && r->accepted;
}
static int scope_equal(flow_scope a,flow_scope b) {
    return a.ui==b.ui && a.input==b.input && a.world==b.world;
}
static void cancel_current(uint32_t generation) {
    dm_android_touch_cancel_at_generation(generation);
    dm_ui_drag_end();dm_ui_touch(3,0,0);
}
static void ghost_sync(const capture *c){
    if(!dm_vk_ui_ghost_request)return;
    const touch_flow *f=&c->flow;const dm_tc_state *s=&c->controls;
    float grid[4];
    int on=f->active&&!f->end_pending&&f->have_scope&&pointer_is(s,f->slot)&&
        s->finger[f->slot].shop_drag&&f->width>0&&f->height>0&&
        atomic_load_explicit(&ghost_live_contact,memory_order_acquire)==f->serial&&dm_ui_shelf_grid(grid);
    if(!on){dm_vk_ui_ghost_request(0,0,0,NULL,0,0,0,NULL,0);return;}
    float lift=DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(f->width,f->height);
    int dim=f->kind!=8||dm_ui_stack_left()==1;
    dm_vk_ui_ghost_request(1,f->ox/f->width,f->oy/f->height,grid,f->x/f->width,(f->y-lift)/f->height,dim,&ghost_live_contact,f->serial);
}
typedef struct {touch_flow expected;int verdict,point;float x,y;int finish;} finish;
static void finish_edit(dm_tc_state *s,void *opaque) {
    finish *f=opaque;
    if(!same(&f->expected) || !flow.end_pending || !pointer_is(s,flow.slot))return;
    dm_tc_finger *finger=&s->finger[flow.slot];
    if(flow.kind<0) {dm_tc_cancel_finger(s,flow.slot);flow.active=flow.end_pending=0;return;}
    if(finger->shop_drag) {
        /* Buying/withdrawing is queued separately only after this edit has
         * revalidated the contact. Keep the cursor at the origin goods. */
        s->pointer_x=flow.ox;s->pointer_y=flow.oy;
    } else if(finger->pointer_dragged) {
        if(f->point)dm_tc_pointer_release_at(s,f->x*flow.width,f->y*flow.height);
        else dm_tc_pointer_release(s,f->verdict==7?0:f->verdict);
    }
    f->finish=1;
}

void dm_android_d2r_touch_flow_pump(void) {
    dm_android_touch_snapshot snapshot;dm_android_touch_snapshot_get(&snapshot);
    capture c;if(!capture_now(&c,snapshot.cancel_generation))return;
    if(c.flow.cleanup) {
        dm_ui_drag_end();dm_ui_touch(3,0,0);
        reply clean={.expected=c.flow,.mode=REPLY_CLEAN};apply(&clean);
    }
    if(!c.flow.active || !snapshot.ready || !snapshot.enabled){ghost_sync(&(capture){0});return;}
    if(!pointer_is(&c.controls,c.flow.slot)) {
        reply abort={.expected=c.flow,.mode=REPLY_ABORT};apply(&abort);ghost_sync(&(capture){0});return;
    }
    dm_android_input_status input;dm_android_input_status_get(&input);
    dm_android_d2r_scene_status scene;dm_android_d2r_scene_status_get(&scene);
    flow_scope scope={.ui=dm_ui_gen(),.input=input.input_resets,.world=scene.world_epoch};
    if(!input.source_ready || input.controller_mode || dm_padmode_busy() ||
       (c.flow.have_scope && !scope_equal(c.flow.scope,scope))) {
        cancel_current(snapshot.cancel_generation);ghost_sync(&(capture){0});return;
    }
    double now=now_ns()/1e9;
    reply r={.expected=c.flow,.scope=scope,.mode=REPLY_SCOPE,.now=now};apply(&r);
    if(capture_now(&c,snapshot.cancel_generation))ghost_sync(&c);
    if(c.flow.origin_pending) {
        int result=dm_ui_item_result(c.flow.origin_seq);
        if(result>=0) {
            r.mode=REPLY_ORIGIN;r.result=result;r.classified=0;apply(&r);
            if(r.classified && dm_android_skill_flow_classified)
                dm_android_skill_flow_classified(c.flow.generation,c.flow.slot,r.contact_start);
        }
    }
    if(!capture_now(&c,snapshot.cancel_generation) || !c.flow.active)return;
    ghost_sync(&c);
    if(c.flow.need_origin) {
        r=(reply){.expected=c.flow,.mode=REPLY_ORIGIN_QUERY,
            .seq=dm_ui_item_query(c.flow.ox/c.flow.width,c.flow.oy/c.flow.height)};apply(&r);
        return;
    }
    if(c.flow.origin_pending) {
        if(c.flow.end_pending && now-c.flow.end_wall>=0.25)cancel_current(snapshot.cancel_generation);
        return;
    }
    if(c.flow.want_drag) {
        if(!c.flow.host_drag) {
            dm_ui_drag_lift(DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(c.flow.width,c.flow.height)/c.flow.height);
            dm_ui_drag_begin(c.flow.want_drag,c.flow.ox/c.flow.width,c.flow.oy/c.flow.height,44.0f/c.flow.height);
        }
        dm_ui_drag_move(c.flow.x/c.flow.width,c.flow.y/c.flow.height);
        r=(reply){.expected=c.flow,.mode=REPLY_DRAG,.frame=dm_guest_frame()};
        if(!apply(&r)) {dm_ui_drag_end();dm_ui_touch(3,0,0);return;}
    }
    if(c.flow.drop_pending) {
        int result=dm_ui_item_result(c.flow.drop_seq);
        if(result>=0) {
            r=(reply){.expected=c.flow,.mode=REPLY_DROP,.result=result,.bag=dm_ui_drop_bag()};
            if(result==5)r.point=dm_ui_drop_point(&r.x,&r.y);
            r.origin=dm_ui_origin_point(&r.origin_x,&r.origin_y);apply(&r);
        }
    }
    if(!capture_now(&c,snapshot.cancel_generation) || !c.flow.active)return;
    if(c.flow.need_drop && !c.flow.drop_pending) {
        float lift=DM_TC_POINTER_LIFT_PT*dm_tc_scale_for_size(c.flow.width,c.flow.height);
        r=(reply){.expected=c.flow,.mode=REPLY_DROP_QUERY,
            .seq=dm_ui_drop_query(c.flow.x/c.flow.width,(c.flow.y-lift)/c.flow.height,c.flow.y/c.flow.height)};
        apply(&r);return;
    }
    if(!c.flow.end_pending)return;
    /* mouse_pump runs before this pump, once per rendered frame. Keep a
     * completed UI swipe live until it has consumed the final coordinates,
     * including a quick swipe whose origin answer arrived after release. */
    if(c.flow.want_drag && c.flow.host_drag_frame==dm_guest_frame() && now-c.flow.end_wall<0.25)return;
    if(c.flow.dragged && (!c.flow.drop_final || c.flow.drop_pending) && now-c.flow.end_wall<0.25)return;
    /* A historical answer must never authorize a purchase, sale or drop at
     * a different final UP point. Missing final confirmation returns an
     * already-picked-up item to its origin; shelf/stack drags do no trade. */
    int confirmed=c.flow.drop_final && !c.flow.drop_pending && c.flow.last_drop_final;
    int verdict=confirmed && c.flow.last_drop>=0?c.flow.last_drop:0;
    finish f={.expected=c.flow,.verdict=verdict,.point=verdict==5 && c.flow.last_point,
        .x=c.flow.drop_x,.y=c.flow.drop_y};
    if(verdict==0 && dm_ui_origin_point(&f.x,&f.y))f.point=1;
    dm_android_touch_edit(c.flow.generation,finish_edit,&f);
    if(!f.finish)return;
    /* Same production actions as iOS. Scope/contact are checked again before
     * queuing any trade; the original host revalidates the selected item. */
    dm_android_touch_snapshot latest;dm_android_touch_snapshot_get(&latest);
    if(latest.cancel_generation!=c.flow.generation || dm_ui_gen()!=scope.ui)return;
    if(c.controls.finger[c.flow.slot].shop_drag &&
       ((verdict==5 && c.flow.last_bag) || (verdict==11 && c.flow.kind==8))) {
        if(verdict==11)dm_ui_stack_cube_at(c.flow.ox/c.flow.width,c.flow.oy/c.flow.height);
        else if(c.flow.kind==8)dm_ui_stack_take_at(c.flow.ox/c.flow.width,c.flow.oy/c.flow.height);
        else dm_ui_shop_buy_at(c.flow.ox/c.flow.width,c.flow.oy/c.flow.height);
    } else if(c.controls.finger[c.flow.slot].pointer_dragged && verdict==7)dm_shop_sell_cursor();
    dm_android_touch_finish_pending(c.flow.generation,c.flow.slot,c.flow.end_time);
    if(c.flow.host_drag)dm_ui_drag_end();
}
