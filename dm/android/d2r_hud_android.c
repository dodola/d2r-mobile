#include "../host/dm.h"
#include "../host/dm_hud.h"
#include "../host/dm_cursor.h"
#include "../host/dm_input.h"
#include "../host/dm_actions.h"
#include "../host/dm_i18n.h"
#include "../ios/Sources/dm_touch_game_identity.h"
#include "../ios/Sources/dm_touch_native_art.h"
#include "d2r_hud_android.h"
#include "touch_controls_android.h"
#include "input_backend.h"
#include <jni.h>
#include <stdatomic.h>
#include <math.h>

static pthread_mutex_t drawn_lock=PTHREAD_MUTEX_INITIALIZER;
static int64_t last_drawn[DM_AHUD_WORDS];
static uint32_t drawn_mask;
static uint64_t drawn_at;
static _Atomic int hud_blocked;
#if DM_ANDROID_NATIVE_ART
static const uint64_t buttons[DM_AHUD_KEYS]={DM_BTN_A,DM_BTN_B,DM_BTN_X,DM_BTN_Y,DM_BTN_R};
static void copy_glyph(int64_t *v,dm_ta_glyph g) {
    v[0]=g.status;v[1]=g.atlas;v[8]=g.raw_skill_id;v[9]=g.slot;v[10]=g.effective_slot;
    dm_ta_frame normal,pressed;
    if(g.status==DM_TA_GLYPH && dm_ta_frame_at(g.atlas,g.normal_frame,&normal) && dm_ta_frame_at(g.atlas,g.pressed_frame,&pressed)) {
        v[2]=normal.x;v[3]=normal.y;v[4]=normal.width;v[5]=normal.height;v[6]=pressed.x;v[7]=pressed.y;
    } else if(g.status==DM_TA_GLYPH)v[0]=DM_TA_UNKNOWN;
}
#endif
void dm_android_hud_message(const char *action,const char *argument) {
    int dm_hud_stack_msg(const char *,const char *);
    atomic_store(&hud_blocked,dm_hud_stack_msg(action,argument));
}
void dm_android_hud_frame(int64_t out[DM_AHUD_WORDS]) {
    memset(out,0,DM_AHUD_WORDS*sizeof *out);out[0]=DM_AHUD_VERSION;
    dm_hud_info h;dm_hud_get(&h);
    dm_android_touch_game_state state;dm_android_touch_get_game_state(&state);
    dm_android_touch_snapshot touch;dm_android_touch_snapshot_get(&touch);
    const dm_tc_state *controls=&touch.controls;
    int set=controls->skill_set;uint64_t held=controls->buttons;
    int enabled=touch.enabled && touch.ready;
    int blocked=atomic_load(&hud_blocked);
    out[1]=enabled && dm_hud_bar_visible(&h,state.navigation && blocked>=0,blocked>0);
    out[2]=h.valid;out[3]=h.level;out[4]=h.exp_permille;
    out[5]=h.hp;out[6]=h.hp_max;out[7]=h.mp;out[8]=h.mp_max;
    for(int i=0;i<4;i++)out[9+i]=h.belt[i];
    out[13]=h.players;out[14]=h.points;out[15]=h.quest_unread;out[16]=h.dead;
    out[17]=set;out[18]=dm_lang_zh();out[19]=(int64_t)held;out[27]=enabled;
    out[DM_AHUD_NAVIGATION]=controls->navigation_mode;
    out[DM_AHUD_DIFF_OPEN]=controls->diff_open;
    out[DM_AHUD_MAP_CANCEL]=controls->map_cancel_active;
    out[DM_AHUD_TARGETS]=controls->enabled_targets;
    out[DM_AHUD_JOY_X]=llroundf(controls->joy_origin_x*1000.f);
    out[DM_AHUD_JOY_Y]=llroundf(controls->joy_origin_y*1000.f);
    for(int i=0;i<16;i++)if(controls->finger[i].active && controls->finger[i].target==DM_TC_SWAP)
        out[DM_AHUD_SWAP_HELD]=1;
    dm_tg_snapshot n;dm_touch_game_snapshot(&n);
    out[20]=(int64_t)n.captured_ns;out[21]=(int64_t)n.scene_epoch;out[22]=(int64_t)n.world_epoch;
    out[23]=(int64_t)n.binding_identity;out[24]=(int64_t)n.hud_identity;out[25]=n.selected_bank;
#if DM_ANDROID_NATIVE_ART
    out[26]=1;uint64_t now=now_ns();
    for(int i=0;i<DM_AHUD_KEYS;i++) {
        dm_ta_glyph g=dm_ta_current_set(&n,buttons[i],now,set);
        /* Match the iOS visual rule: a stalled snapshot retains its image.
         * Hide/input eligibility still requires the fresh original snapshot. */
        if(!g.status && n.captured_ns && now>n.captured_ns && now-n.captured_ns>DM_TG_MAX_AGE_NS)
            g=dm_ta_current_set(&n,buttons[i],n.captured_ns,set);
        int64_t *v=out+DM_AHUD_HEAD+i*DM_AHUD_GLYPH;
        copy_glyph(v,g);
        if(g.status==DM_TA_GLYPH) {
            int charges=0,kind=dm_swapskill_slot(g.slot,&charges);
            v[11]=(int64_t)((uint64_t)(uint32_t)kind|((uint64_t)(uint32_t)charges<<32));
        }
    }
    copy_glyph(out+DM_AHUD_PORTAL,dm_ta_skill(359));
#else
    (void)n;
#endif
}
void dm_android_hud_rendered(const int64_t *words,size_t count,uint32_t drawn) {
    pthread_mutex_lock(&drawn_lock);
    drawn_mask=0;drawn_at=0;
    if(words && count==DM_AHUD_WORDS && words[0]==DM_AHUD_VERSION && words[1] && words[2] && words[26] && words[27]) {
        memcpy(last_drawn,words,sizeof last_drawn);drawn_mask=drawn&31;drawn_at=now_ns();
    }
    pthread_mutex_unlock(&drawn_lock);
}
int dm_android_hud_receipt_allows(const dm_tg_snapshot *n,unsigned slot,uint64_t now) {
#if DM_ANDROID_NATIVE_ART
    if(!n || slot>=6 || !n->supported || !n->valid || !n->ready || n->source!=DM_TG_SOURCE_NATIVE_GETTER ||
       n->identity_scope!=DM_TG_IDENTITY_NATIVE || !n->captured_ns || now<n->captured_ns || now-n->captured_ns>DM_TG_MAX_AGE_NS)return 0;
    int allowed=0;pthread_mutex_lock(&drawn_lock);
    if(drawn_at && now>=drawn_at && now-drawn_at<=DM_TG_MAX_AGE_NS && last_drawn[17]==0 &&
       last_drawn[20]>0 && (uint64_t)last_drawn[20]<=n->captured_ns && now-(uint64_t)last_drawn[20]<=DM_TG_MAX_AGE_NS &&
       (uint64_t)last_drawn[21]==n->scene_epoch && (uint64_t)last_drawn[22]==n->world_epoch &&
       (uint64_t)last_drawn[23]==n->binding_identity && (uint64_t)last_drawn[24]==n->hud_identity && last_drawn[25]==n->selected_bank) {
        for(int i=0;i<DM_AHUD_KEYS;i++)if(drawn_mask&(1u<<i)) {
            const int64_t *v=last_drawn+DM_AHUD_HEAD+i*DM_AHUD_GLYPH;
            dm_ta_glyph g=dm_ta_current_set(n,buttons[i],now,0);
            if(g.status==DM_TA_GLYPH && v[0]==DM_TA_GLYPH && g.slot==(int)slot &&
               v[9]==g.slot && v[10]==g.effective_slot && (uint32_t)v[8]==g.raw_skill_id && v[1]==g.atlas)allowed=1;
        }
    }
    pthread_mutex_unlock(&drawn_lock);return allowed;
#else
    (void)n;(void)slot;(void)now;return 0;
#endif
}
int dm_android_hud_hide_glyph(uint64_t widget) {
    if(!widget || !dm_android_touch_visual_state(NULL,NULL,NULL) || dm_android_input_controller_mode())return 0;
    dm_tg_snapshot n;dm_touch_game_snapshot(&n);uint32_t slot=UINT32_MAX;
    if(widget>UINT64_MAX-0xb18 || !dm_tg_vm_read(widget+0xb18,&slot,sizeof slot) ||
       !dm_android_hud_receipt_allows(&n,slot,now_ns()))return 0;
    int dm_touch_game_hide_own_glyph(uint64_t);
    return dm_touch_game_hide_own_glyph(widget);
}
JNIEXPORT jlongArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeHudFrame(JNIEnv *env,jclass cls) {
    (void)cls;int64_t frame[DM_AHUD_WORDS];dm_android_hud_frame(frame);
    jlongArray out=(*env)->NewLongArray(env,DM_AHUD_WORDS);
    if(out)(*env)->SetLongArrayRegion(env,out,0,DM_AHUD_WORDS,(const jlong *)frame);return out;
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeHudRendered(JNIEnv *env,jclass cls,jlongArray frame,jint mask) {
    (void)cls;int64_t copy[DM_AHUD_WORDS];
    if(!frame || (*env)->GetArrayLength(env,frame)!=DM_AHUD_WORDS) {dm_android_hud_rendered(NULL,0,0);return;}
    (*env)->GetLongArrayRegion(env,frame,0,DM_AHUD_WORDS,(jlong *)copy);
    if(!(*env)->ExceptionCheck(env))dm_android_hud_rendered(copy,DM_AHUD_WORDS,(uint32_t)mask);
}
