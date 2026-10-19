/* Android's game-thread counterpart of front_ios.m's skillKeyAt/bindTapSync.
 * No UI callback reads guest memory or calls the guest. All shared state below
 * is protected by touch_controls_android's lock through event/edit callbacks. */
#include "d2r_touch_skill_flow_android.h"
#include "d2r_host_modules.h"
#include "input_backend.h"
#include "../host/dm.h"
#include "../host/dm_actions.h"
#include "../host/dm_cursor.h"
#include "../host/dm_input.h"
#include "../ios/Sources/dm_touch_native_art.h"
#include <math.h>

extern int dm_padmode_busy(void);

typedef struct {
    uint32_t generation,ui;
    uint64_t input,world,scene,binding,hud;
    int bank;
} skill_scope;
typedef struct {
    skill_scope scope;
    dm_tg_snapshot native;
    dm_points_info points;
    uint64_t now_ns,tick;
    int valid,game,navigation,bind,cursor_pending,focused_skill;
} observation;
typedef struct {
    skill_scope scope;
    int active,empty,classified,hk,released;
    uint32_t hit;
    uint64_t key;
    float nx,ny;
    double start,next_stat;
    unsigned before_move;
} contact;
typedef struct {
    observation cache;
    uint64_t revision;
    uint64_t explicit_cancel_epoch;
    contact finger[DM_TC_MAX_TOUCH];
    uint32_t selected;
    int selected_skill;
    skill_scope selected_scope;
    uint64_t selected_tick;
    double add_at;
    int add_skill;
    skill_scope add_scope;
    int stat_request;
    skill_scope stat_scope;
    uint64_t bind_request;
    skill_scope bind_scope;
    uint64_t own_y_epoch;
    skill_scope y_scope;
} skill_flow;
static skill_flow flow={.selected_skill=-1,.add_skill=-1,.stat_request=-1};

static int scope_equal(skill_scope a,skill_scope b) {
    return a.generation==b.generation && a.ui==b.ui && a.input==b.input &&
        a.world==b.world && a.scene==b.scene && a.binding==b.binding &&
        a.hud==b.hud && a.bank==b.bank;
}
static void clear_selection(void) {
    flow.selected=0;flow.selected_skill=-1;flow.add_at=0;flow.add_skill=-1;
}
static int is_pointer(const dm_tc_state *s,int slot) {
    return s->navigation_mode && s->pointer_slot==slot && s->finger[slot].active;
}
static int empty_key(const dm_tc_state *s,int target,const observation *o,uint64_t *key) {
    if(target<DM_TC_MAIN || target>DM_TC_SKILL4 ||
       !(s->enabled_targets&(1u<<target)) || !o->valid || !o->game || o->navigation)return 0;
#if DM_ANDROID_NATIVE_ART
    dm_ta_glyph glyph=dm_ta_current_set(&o->native,s->layout.target[target].button,o->now_ns,s->skill_set);
    if(glyph.status!=DM_TA_EMPTY)return 0;
    *key=s->layout.target[target].button|(s->skill_set?DM_BTN_ZL:0);return 1;
#else
    (void)key;return 0;
#endif
}
static void rewrite_tap(dm_tc_state *s,const contact *c) {
    s->tap_button=0;s->tap_move=0;
    if(!c->classified || !c->hk || !flow.cache.valid ||
       !scope_equal(c->scope,flow.cache.scope))return;
    s->tap_x=c->nx*s->layout.width;s->tap_y=c->ny*s->layout.height;
    if(c->hk==1 && flow.cache.bind==2)s->tap_button=DM_BTN_Y;
    else if(!flow.cache.bind)s->tap_move=1;
}
int dm_android_skill_flow_event(dm_tc_state *s,uint32_t generation,int slot,int phase,double now) {
    if(phase==DM_ATF_RESET) {
        ++flow.revision;
        if(slot!=DM_ATF_RESET_SCENE)++flow.explicit_cancel_epoch;
        memset(flow.finger,0,sizeof flow.finger);clear_selection();
        flow.stat_request=-1;flow.bind_request=0;flow.cache.valid=0;flow.own_y_epoch=0;
        s->tap_button=0;s->tap_move=0;return 0;
    }
    if(slot<0 || slot>=DM_TC_MAX_TOUCH)return 0;
    contact *c=&flow.finger[slot];dm_tc_finger *f=&s->finger[slot];
    if(phase==DM_ATF_BEGIN) {
        ++flow.revision;
        /* A fresh contact invalidates a delayed point spend, even at the same
         * pixel. Selection itself survives until this origin is classified. */
        flow.add_at=0;flow.add_skill=-1;
        *c=(contact){.active=1,.scope=flow.cache.scope,.start=f->start_time_s};
        c->scope.generation=generation;
        uint64_t key=0;observation current=flow.cache;current.now_ns=(uint64_t)(now*1e9);
        if(!s->navigation_mode && empty_key(s,f->target,&current,&key)) {
            dm_tc_finger original=*f;dm_tc_cancel_finger(s,slot);
            /* Retain a harmless active contact so MOVE/UP keep their final
             * coordinates; leaving it inactive loses move-out cancellation. */
            original.target=DM_TC_EDGE;original.key_down=0;original.aim_cancelled=1;
            s->finger[slot]=original;c->empty=1;c->key=key;
        } else if(!is_pointer(s,slot))c->active=0;
        return 0;
    }
    if(!c->active || c->scope.generation!=generation)return 0;
    if(phase==DM_ATF_RELEASE)c->released=1;
    if(phase==DM_ATF_MOVE && (f->pointer_dragged || f->ui_drag ||
       hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)>10*dm_tc_scale_for_size(s->layout.width,s->layout.height))) {
        ++flow.revision;
        flow.add_at=0;flow.add_skill=-1;
    }
    if(phase==DM_ATF_END) {
        c->before_move=s->tap_move_seq;
        if(c->empty) {
            if(flow.cache.valid && !s->navigation_mode && !flow.cache.bind &&
               scope_equal(c->scope,flow.cache.scope) &&
               hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)<=14) {
                flow.bind_request=c->key;flow.bind_scope=c->scope;
            }
        } else if(is_pointer(s,slot)) {
            /* An unclassified/stale point must never fall through to the
             * original mouse handler, which spends points immediately. */
            if(!c->classified || !flow.cache.valid || !scope_equal(c->scope,flow.cache.scope)) {
                dm_tc_cancel_finger(s,slot);clear_selection();
            } else {
                rewrite_tap(s,c);
                if(s->tap_button && (s->pulse_button || s->pointer_pulse_pending))
                    dm_tc_cancel_finger(s,slot);
            }
        }
    }
    if(phase==DM_ATF_AFTER_END) {
        if(!c->empty && s->tap_move_seq!=c->before_move && c->hit) {
            if(c->hit==flow.selected && scope_equal(c->scope,flow.selected_scope)) {
                if(c->hk==2 && c->hit<=4) {
                    flow.stat_request=(int)c->hit-1;flow.stat_scope=c->scope;
                } else if(c->hk==1 && flow.selected_skill>=0) {
                    flow.add_at=now+.3;flow.add_skill=flow.selected_skill;flow.add_scope=c->scope;
                }
            } else {
                flow.selected=c->hit;flow.selected_skill=-1;
                flow.selected_scope=c->scope;flow.selected_tick=flow.cache.tick;
            }
        }
        if(s->tap_button && s->pulse_button==DM_BTN_Y) {
            flow.own_y_epoch=s->pulse_epoch;flow.y_scope=c->scope;
        }
        c->active=0;s->tap_button=0;s->tap_move=0;
    }
    return 0;
}

typedef struct {
    observation o;
    int classify,slot,hk;
    uint32_t hit;
    float nx,ny;
    double contact_start;
    int stat,skill,skill_id;
    uint64_t revision;
    uint64_t bind;
    skill_scope action_scope;
} update;
static void update_edit(dm_tc_state *s,void *opaque) {
    update *u=opaque;observation old=flow.cache;
    if(u->classify)u->o.tick=old.tick;
    flow.cache=u->o;
    if(flow.own_y_epoch && (!u->o.valid || !scope_equal(flow.y_scope,u->o.scope))) {
        dm_tc_pulse_end(s,flow.own_y_epoch);flow.own_y_epoch=0;
    }
    if(flow.selected && (!u->o.valid || !u->o.game || !scope_equal(flow.selected_scope,u->o.scope)))clear_selection();
    for(int slot=0;slot<DM_TC_MAX_TOUCH;slot++) {
        contact *c=&flow.finger[slot];dm_tc_finger *f=&s->finger[slot];
        if(!c->active)continue;
        if(!u->o.valid || !scope_equal(c->scope,u->o.scope) ||
           !f->active || f->start_time_s!=c->start) {
            if(f->active && f->start_time_s==c->start)dm_tc_cancel_finger(s,slot);
            c->active=0;flow.add_at=0;continue;
        }
        if(u->classify && u->slot==slot && c->start==u->contact_start && is_pointer(s,slot)) {
            c->classified=1;c->hk=u->hk;c->hit=u->hit;c->nx=u->nx;c->ny=u->ny;
            if(!c->hk || (flow.selected && flow.selected!=c->hit))clear_selection();
            rewrite_tap(s,c);
        }
        if(u->classify)continue;
        if(!u->o.game || c->empty || !c->classified || c->released || !is_pointer(s,slot) || u->o.bind ||
           f->pointer_dragged || f->ui_drag || u->o.now_ns/1e9-f->start_time_s<.5 ||
           hypotf(f->x-f->aim_origin_x,f->y-f->aim_origin_y)>10*dm_tc_scale_for_size(s->layout.width,s->layout.height))continue;
        if(c->hk==2 && c->hit>=1 && c->hit<=4 && u->o.points.char_open && u->o.points.stat_points>0) {
            if(!f->pointer_long) {
                f->pointer_long=1;c->next_stat=0;flow.selected=c->hit;flow.selected_skill=-1;
                flow.selected_scope=c->scope;flow.add_at=0;
                s->pointer_valid=1;s->pointer_x=c->nx*s->layout.width;s->pointer_y=c->ny*s->layout.height;
            }
            double now=u->o.now_ns/1e9;
            if(now>=c->next_stat) {c->next_stat=now+.15;u->stat=(int)c->hit-1;u->action_scope=c->scope;}
        } else if(c->hk==1 && u->o.points.tree_open && !f->pointer_long &&
                  !s->pulse_button && !s->pointer_pulse_pending) {
            dm_tc_pointer_press(s,c->nx*s->layout.width,c->ny*s->layout.height,DM_BTN_Y);
            flow.own_y_epoch=s->pulse_epoch;flow.y_scope=c->scope;clear_selection();
        }
    }
    if(u->classify)return;
    if(u->o.bind==2 && old.bind!=2 && u->o.valid) {
        s->pointer_valid=1;s->pointer_x=.155f*s->layout.width;s->pointer_y=.5f*s->layout.height;
    }
    if(flow.selected>4 && flow.selected_skill<0 && u->o.tick>flow.selected_tick &&
       !u->o.cursor_pending && u->o.focused_skill>=0 && u->o.points.tree_open &&
       u->o.points.sel_skill==u->o.focused_skill)flow.selected_skill=u->o.focused_skill;
    if(flow.stat_request>=0) {
        if(u->o.valid && u->o.game && !u->o.bind && scope_equal(flow.stat_scope,u->o.scope) &&
           u->o.points.char_open && u->o.points.stat_points>0) {
            u->stat=flow.stat_request;u->action_scope=flow.stat_scope;
        }
        flow.stat_request=-1;
    }
    double now=u->o.now_ns/1e9;
    if(flow.add_at>0 && now>=flow.add_at) {
        if(!u->o.valid || !u->o.game || u->o.bind || !u->o.points.tree_open ||
           !scope_equal(flow.add_scope,u->o.scope) || now>flow.add_at+1)flow.add_at=0;
        else if(!u->o.cursor_pending && u->o.focused_skill==flow.add_skill &&
                u->o.points.sel_skill==flow.add_skill) {
            if(u->o.points.sel_block==0 || u->o.points.sel_block==5) {
                u->skill=1;u->skill_id=flow.add_skill;u->action_scope=flow.add_scope;
            }
            flow.add_at=0;
        }
    }
    if(flow.bind_request) {
        if(u->o.valid && u->o.game && !u->o.navigation && !u->o.bind && scope_equal(flow.bind_scope,u->o.scope)) {
            /* Recheck the exact normal/alt slot against a fresh native table,
             * rather than allowing a cached empty glyph to rebind a new skill. */
#if DM_ANDROID_NATIVE_ART
            dm_ta_glyph glyph=dm_ta_current_set(&u->o.native,flow.bind_request&~(uint64_t)DM_BTN_ZL,
                u->o.now_ns,!!(flow.bind_request&DM_BTN_ZL));
            if(glyph.status==DM_TA_EMPTY) {u->bind=flow.bind_request;u->action_scope=flow.bind_scope;}
#endif
        }
        flow.bind_request=0;
    }
    u->revision=flow.revision;
}
typedef struct {uint32_t selected;uint64_t explicit_cancel_epoch;} selection_snapshot;
static void selection_edit(dm_tc_state *s,void *opaque) {
    (void)s;*(selection_snapshot *)opaque=(selection_snapshot){flow.selected,flow.explicit_cancel_epoch};
}
static void dispatch_guard_edit(dm_tc_state *s,void *opaque) {
    (void)s;update *u=opaque;
    if(flow.revision!=u->revision || !flow.cache.valid ||
       !scope_equal(flow.cache.scope,u->action_scope)) {u->stat=-1;u->skill=0;u->bind=0;}
}
static observation observe(uint32_t generation,uint32_t selected,uint64_t tick) {
    observation o={0};o.tick=tick;o.focused_skill=-1;
    dm_android_input_status input;dm_android_input_status_get(&input);
    dm_android_d2r_scene_status scene;dm_android_d2r_scene_status_get(&scene);
    dm_touch_game_snapshot(&o.native);dm_points_get(&o.points);
    o.game=scene.game;o.navigation=scene.navigation;o.bind=dm_ui_bind_active();
    o.cursor_pending=dm_cursor_pending();
    o.scope=(skill_scope){.generation=generation,.ui=dm_ui_gen(),.input=input.input_resets,
        .world=scene.world_epoch,.scene=scene.scene_epoch,.binding=o.native.binding_identity,
        .hud=o.native.hud_identity,.bank=o.native.selected_bank};
    o.valid=scene.attached && input.source_ready && !input.controller_mode &&
        !dm_padmode_busy() && !dm_ui_unstable();
    if(o.valid && o.game && selected>4)o.focused_skill=dm_ui_hit_selected_skill(selected);
    /* A native observation can be published while these copies are read.
     * Measure freshness after the copy, never against an earlier timestamp. */
    o.now_ns=now_ns();
    return o;
}
void dm_android_skill_flow_classified(uint32_t generation,int slot,double contact_start) {
    if(slot<0 || slot>=DM_TC_MAX_TOUCH)return;
    selection_snapshot selection={0};if(!dm_android_touch_edit(generation,selection_edit,&selection))return;
    update u={.o=observe(generation,selection.selected,0),.classify=1,.slot=slot,.contact_start=contact_start,.stat=-1};
    u.hk=dm_ui_bind_skill_hit(&u.nx,&u.ny);u.hit=dm_ui_hit_id();
    /* A completion may arrive after a new contact reused the same slot. The
     * edit checks start_time as well as cancellation generation before use. */
    dm_android_touch_edit(generation,update_edit,&u);
}
void dm_android_skill_flow_pump(void) {
    /* Ownership is game-thread local. Expected panel/UI changes while opening
     * our tree do not cancel binding; input/world/lifecycle changes do. */
    static struct {int active;skill_scope scope;uint64_t tick,cancel_epoch;} owner;
    static uint64_t tick;
    dm_android_touch_snapshot snapshot;dm_android_touch_snapshot_get(&snapshot);
    selection_snapshot selection={0};
    if(!dm_android_touch_edit(snapshot.cancel_generation,selection_edit,&selection))return;
    update u={.o=observe(snapshot.cancel_generation,selection.selected,++tick),.stat=-1};
    u.o.valid&=snapshot.ready && snapshot.enabled;
    if(owner.active) {
        if(!snapshot.enabled || !snapshot.ready || !u.o.valid || !u.o.game ||
           owner.cancel_epoch!=selection.explicit_cancel_epoch || owner.scope.input!=u.o.scope.input ||
           owner.scope.world!=u.o.scope.world) {dm_ui_bind_cancel();owner.active=0;}
        else if(!u.o.bind && tick>owner.tick)owner.active=0;
    }
    if(!dm_android_touch_edit(snapshot.cancel_generation,update_edit,&u))return;
    if(u.stat<0 && !u.skill && !u.bind)return;
    dm_android_touch_snapshot latest;dm_android_touch_snapshot_get(&latest);
    observation check=observe(latest.cancel_generation,selection.selected,tick);
    if(!latest.enabled || !latest.ready || !check.valid || !scope_equal(check.scope,u.action_scope))return;
    if(!dm_android_touch_edit(latest.cancel_generation,dispatch_guard_edit,&u))return;
    if(u.stat>=0 && !check.bind && check.points.char_open && check.points.stat_points>0)dm_stat_add(u.stat,1);
    if(u.skill && !check.bind && check.points.tree_open && check.focused_skill==u.skill_id &&
       check.points.sel_skill==u.skill_id && !check.cursor_pending)dm_skill_add_selected();
    if(u.bind && !check.bind && !check.navigation) {
        dm_ui_bind_begin(u.bind);owner.active=1;owner.scope=check.scope;owner.tick=tick;
        owner.cancel_epoch=selection.explicit_cancel_epoch;
    }
}
