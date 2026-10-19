#include "d2r_world_overlay_android.h"
#include "d2r_custom_android.h"
#include "services_platform.h"
#include "../host/dm_hud.h"
#include "../host/dm_rift.h"
#include "../host/dm_world.h"
#include "../host/dm_cursor.h"
#include "../host/dm_i18n.h"
#include "../host/dm_uiscreen.h"
#include "../ios/Sources/dm_touch_game_state.h"
#include <jni.h>
#include <math.h>
#include <stdatomic.h>

static pthread_mutex_t publication_lock=PTHREAD_MUTEX_INITIALIZER;
static dm_android_world_overlay_snapshot published;
static _Atomic uint64_t message_epoch=1;
static int old_names=-1;
static int64_t fixed(double value){return llround(value*1000000.0);}
static void empty(dm_android_world_overlay_snapshot *out){memset(out,0,sizeof *out);out->word[0]=DM_AWORLD_VERSION;}
static void rift_text(dm_android_world_overlay_snapshot *out,const dm_rift_hud_t *h) {
    char (*t)[DM_AWORLD_TEXT_BYTES]=out->text;int seconds=h->secs_left<0?0:h->secs_left;
    snprintf(t[0],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudTitle"));
    if(h->done)snprintf(t[1],DM_AWORLD_TEXT_BYTES,dm_ui(h->inc?"dmRiftHudDone":"dmRiftHudLate"),h->tier);
    else if(h->guardian)snprintf(t[1],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudGuard"));
    else snprintf(t[1],DM_AWORLD_TEXT_BYTES,dm_ui("dmRiftTierN"),h->tier);
    snprintf(t[2],DM_AWORLD_TEXT_BYTES,"%d:%02d",seconds/60,seconds%60);
    if(h->done) {
        if(!h->inc)snprintf(t[3],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudSame"));
        else if(h->next>h->tier)snprintf(t[3],DM_AWORLD_TEXT_BYTES,dm_ui("dmRiftHudNext"),h->next);
        else snprintf(t[3],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudTop"));
    } else if(h->goal==5)snprintf(t[3],DM_AWORLD_TEXT_BYTES,dm_ui("dmRiftHudOverKillN"),h->gdead,h->gtotal);
    else if(h->goal==4)snprintf(t[3],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudOverKill"));
    else if(h->goal==3)snprintf(t[3],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudOver"));
    else if((h->goal==1||h->goal==2)&&h->nwave>1)snprintf(t[3],DM_AWORLD_TEXT_BYTES,dm_ui("dmRiftHudKillWave"),h->wave,h->nwave,h->gdead,h->gtotal);
    else if(h->goal==2)snprintf(t[3],DM_AWORLD_TEXT_BYTES,dm_ui("dmRiftHudKillN"),h->gdead,h->gtotal);
    else if(h->goal==1)snprintf(t[3],DM_AWORLD_TEXT_BYTES,"%s",dm_ui("dmRiftHudKill"));
    else snprintf(t[3],DM_AWORLD_TEXT_BYTES,dm_ui("dmRiftHudGoal"),(int)(h->progress*100));
}
void dm_android_world_overlay_message(const char *action,const char *argument) {
    (void)action;(void)argument;
    /* Do not replay a PanelManager toggle into the shared stack a second
     * time. Invalidate immediately; the next original-thread sample owns
     * the new panel/navigation state and publishes it atomically. */
    atomic_fetch_add(&message_epoch,1);
}
void dm_android_world_overlay_tick(const dm_android_touch_game_state *state) {
    dm_android_world_overlay_snapshot out;empty(&out);
    dm_android_window_state window;dm_android_platform_get_window(&window);
    dm_hud_info hud;dm_hud_get(&hud);
    uint64_t message=atomic_load(&message_epoch);
    if(state && dm_android_title_custom_settings() && hud.valid && !hud.dead && !state->navigation && dm_uiscreen_ingame() &&
       window.focus==1 && window.surface_available) {
        dm_tg_snapshot scene;dm_touch_game_snapshot(&scene);
        int width=0,height=0;dm_screen_real(&width,&height);
        if(width>0&&height>0) {
            out.word[2]=now_ns();out.word[3]=dm_ui_gen();out.word[4]=scene.scene_epoch;out.word[5]=scene.world_epoch;
            out.word[6]=window.generation;out.word[9]=width;out.word[10]=height;
            out.word[25]=dm_lang_index();if(old_names<0){int dm_custom_get(const char *);old_names=dm_custom_get("item_notes")!=0;}
            out.word[26]=old_names;out.word[27]=message;
            dm_rift_hud_t rift;if(dm_rift_hud(&rift)&&isfinite(rift.progress)) {
                out.word[1]|=DM_AWORLD_RIFT;out.word[21]=fixed(fmax(0,fmin(1,rift.progress)));
                out.word[22]=rift.secs_left<0?0:rift.secs_left;out.word[23]=rift.guardian;out.word[24]=rift.done;
                rift_text(&out,&rift);
            }
            float cell=0,mini[4]={0};int mode=dm_ui_map_geom(&cell),have_mini=dm_ui_minimap_rect(mini);
            int valid_mini=have_mini&&isfinite(mini[0])&&isfinite(mini[1])&&isfinite(mini[2])&&isfinite(mini[3])&&
                mini[0]>=0&&mini[1]>=0&&mini[2]<=1&&mini[3]<=1&&mini[2]>mini[0]&&mini[3]>mini[1];
            dm_map_goal_view goals;memset(&goals,0,sizeof goals);int count=dm_map_goals(&goals);
            if((mode==1||mode==2)&&count>0&&count<=DM_AWORLD_GOALS&&isfinite(cell)&&cell>0&&cell<4096&&
               isfinite(goals.sx)&&isfinite(goals.sy)&&fabsf(goals.sx)<16&&fabsf(goals.sy)<16&&
               (mode==2||valid_mini)) {
                out.word[1]|=DM_AWORLD_MAP;out.word[7]=mode;out.word[8]=fixed(cell);out.word[11]=count;
                out.word[12]=goals.px;out.word[13]=goals.py;out.word[14]=fixed(goals.sx);out.word[15]=fixed(goals.sy);out.word[20]=valid_mini;
                if(valid_mini)for(int i=0;i<4;i++)out.word[16+i]=fixed(mini[i]);
                for(int i=0;i<count;i++) {int art=goals.art[i];
                    if(!((art>=0&&art<=30)||(art>=64&&art<=70))){out.word[1]&=~DM_AWORLD_MAP;break;}
                    out.word[DM_AWORLD_HEAD+3*i]=goals.x[i];out.word[DM_AWORLD_HEAD+3*i+1]=goals.y[i];out.word[DM_AWORLD_HEAD+3*i+2]=art;
                }
            }
        }
    }
    pthread_mutex_lock(&publication_lock);published=out;pthread_mutex_unlock(&publication_lock);
}
int dm_android_world_overlay_frame(dm_android_world_overlay_snapshot *out) {
    if(!out)return 0;
    pthread_mutex_lock(&publication_lock);*out=published;pthread_mutex_unlock(&publication_lock);
    dm_android_window_state window;dm_android_platform_get_window(&window);uint64_t now=now_ns();
    dm_tg_snapshot scene;dm_touch_game_snapshot(&scene);
    if(!out->word[1]||!out->word[2]||now<(uint64_t)out->word[2]||now-(uint64_t)out->word[2]>DM_AWORLD_MAX_AGE_NS||
       out->word[3]!=dm_ui_gen()||out->word[4]!=(int64_t)scene.scene_epoch||out->word[5]!=(int64_t)scene.world_epoch||
       out->word[6]!=(int64_t)window.generation||window.focus!=1||!window.surface_available||
       out->word[27]!=(int64_t)atomic_load(&message_epoch)) {empty(out);return 0;}
    return out->word[1]!=0;
}
JNIEXPORT jbyteArray JNICALL Java_org_d2r_binaryprobe_TouchControlsView_nativeWorldFrame(JNIEnv *env,jclass cls) {
    (void)cls;dm_android_world_overlay_snapshot copy;dm_android_world_overlay_frame(&copy);
    jbyteArray result=(*env)->NewByteArray(env,sizeof copy);
    if(result)(*env)->SetByteArrayRegion(env,result,0,sizeof copy,(const jbyte *)&copy);return result;
}
