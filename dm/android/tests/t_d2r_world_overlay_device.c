/* Real snapshot/presentation producer and real i18n table; data-only stand-ins
 * replace game getters. No guest function or player save is executed. */
#include "../d2r_world_overlay_android.h"
#include "../services_platform.h"
#include "../../host/dm_hud.h"
#include "../../host/dm_rift.h"
#include "../../host/dm_world.h"
#include "../../host/dm_i18n.h"
#include "../../ios/Sources/dm_touch_game_state.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks,reads,verified=1,ingame=1,rift_on=1,mode=2,mini_have=1,width=1280,height=720;
static uint32_t ui_generation=4;
static uint64_t clock_ns=1000000000ULL;
static dm_android_window_state window={.focus=1,.surface_available=1,.generation=3};
static dm_hud_info hud={.valid=1};
static dm_tg_snapshot scene={.scene_epoch=8,.world_epoch=9};
static dm_rift_hud_t rift={.tier=42,.progress=.25f,.secs_left=359,.next=43};
static dm_map_goal_view goals={.n=1,.px=100,.py=100,.sx=.5f,.sy=.5f,.x={110},.y={100},.art={64}};
static float cell=4,mini[4]={.7f,.05f,.95f,.3f};
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"world overlay fixture failed line=%d\n",__LINE__);exit(1);}}while(0)
uint64_t now_ns(void){return clock_ns;}
int dm_android_title_custom_settings(void){return verified;}
void dm_android_platform_get_window(dm_android_window_state *out){*out=window;}
void dm_hud_get(dm_hud_info *out){reads++;*out=hud;}
int dm_uiscreen_ingame(void){return ingame;}
int dm_screen_real(int *w,int *h){reads++;*w=width;*h=height;return width>0&&height>0;}
uint32_t dm_ui_gen(void){return ui_generation;}
int dm_touch_game_snapshot(dm_tg_snapshot *out){*out=scene;return 1;}
int dm_custom_get(const char *key){CHECK(!strcmp(key,"item_notes"));return 1;}
int dm_rift_hud(dm_rift_hud_t *out){reads++;*out=rift;return rift_on;}
int dm_ui_map_geom(float *out){reads++;*out=cell;return mode;}
int dm_ui_minimap_rect(float *out){reads++;memcpy(out,mini,sizeof mini);return mini_have;}
int dm_map_goals(dm_map_goal_view *out){reads++;*out=goals;return goals.n;}
static dm_android_touch_game_state state;
static dm_android_world_overlay_snapshot sample(void){dm_android_world_overlay_snapshot out;dm_android_world_overlay_tick(&state);CHECK(dm_android_world_overlay_frame(&out));return out;}
static void hidden(void){dm_android_world_overlay_snapshot out;CHECK(!dm_android_world_overlay_frame(&out));CHECK(out.word[0]==1&&!out.word[1]&&!out.word[2]&&!out.text[0][0]);}
int main(int argc,char **argv) {
    CHECK(argc==1||argc==2);hidden();atomic_store(&g_dm_lang,8);
    dm_android_world_overlay_snapshot out=sample();CHECK(out.word[1]==3&&out.word[7]==2&&out.word[8]==4000000&&out.word[11]==1);
    CHECK(out.word[32]==110&&out.word[33]==100&&out.word[34]==64&&out.word[21]==250000&&out.word[25]==8&&out.word[26]);
    CHECK(!strcmp(out.text[0],"秘境")&&!strcmp(out.text[1],"第 42 层")&&!strcmp(out.text[2],"5:59")&&!strcmp(out.text[3],"消灭秘境中的怪物   25%"));
    int before=reads;CHECK(dm_android_world_overlay_frame(&out));CHECK(reads==before);out.word[32]=999;CHECK(dm_android_world_overlay_frame(&out)&&out.word[32]==110);
    atomic_store(&g_dm_lang,0);out=sample();CHECK(!strcmp(out.text[0],"Rift")&&!strcmp(out.text[1],"Level 42")&&!strcmp(out.text[3],"Slay the monsters   25%"));
    if(argc==2){FILE *f=fopen(argv[1],"wb");CHECK(f);CHECK(fwrite(&out,1,sizeof out,f)==sizeof out);CHECK(!fclose(f));}
    rift.secs_left=42;out=sample();CHECK(!strcmp(out.text[2],"0:42")&&out.word[22]==42);rift.secs_left=41;clock_ns+=1000000000;out=sample();CHECK(!strcmp(out.text[2],"0:41"));
    rift.guardian=1;rift.gdead=1;rift.gtotal=3;rift.wave=2;rift.nwave=1;
    const char *en[]={"Slay the monsters   25%","Slay the Rift Guardian","Slay the Guardians (1/3)","Overtime: drops, no clear","Overtime: slay it for drops","Overtime: slay 1/3 for drops"};
    const char *zh[]={"消灭秘境中的怪物   25%","击败守护者","击败守护者（1/3）","已超时：打完仍有掉落，不计通关","已超时：击败守护者仍有掉落","已超时：击败守护者（1/3）仍有掉落"};
    for(int language=0;language<2;language++){atomic_store(&g_dm_lang,language?8:0);for(int goal=0;goal<=5;goal++){rift.goal=goal;out=sample();CHECK(!strcmp(out.text[1],language?"守护者现身":"Rift Guardian"));CHECK(!strcmp(out.text[3],language?zh[goal]:en[goal]));}}
    rift.goal=2;rift.nwave=4;out=sample();CHECK(!strcmp(out.text[3],"守护者 第 2/4 波（1/3）"));atomic_store(&g_dm_lang,0);out=sample();CHECK(!strcmp(out.text[3],"Guardians 2/4 (1/3)"));
    rift.secs_left=-5;rift.done=1;rift.inc=0;out=sample();CHECK(out.word[22]==0&&!strcmp(out.text[2],"0:00")&&!strcmp(out.text[1],"Level 42 overtime")&&!strcmp(out.text[3],"Next: same level"));
    rift.inc=2;rift.next=44;out=sample();CHECK(!strcmp(out.text[1],"Level 42 complete")&&!strcmp(out.text[3],"Next: Level 44"));rift.tier=100;rift.next=100;out=sample();CHECK(!strcmp(out.text[3],"Highest level cleared"));
    clock_ns+=DM_AWORLD_MAX_AGE_NS;CHECK(dm_android_world_overlay_frame(&out));clock_ns++;hidden();sample();
    clock_ns--;hidden();clock_ns++;sample();ui_generation++;hidden();sample();scene.scene_epoch++;hidden();sample();scene.world_epoch++;hidden();sample();
    dm_android_world_overlay_message("OpenPanel","BankExpansionLayout");hidden();state.navigation=1;dm_android_world_overlay_tick(&state);hidden();state.navigation=0;sample();
    window.focus=3;hidden();window.focus=1;window.generation++;hidden();sample();window.surface_available=0;hidden();window.surface_available=1;sample();
    verified=0;dm_android_world_overlay_tick(&state);hidden();verified=1;ingame=0;dm_android_world_overlay_tick(&state);hidden();ingame=1;hud.valid=0;dm_android_world_overlay_tick(&state);hidden();hud.valid=1;hud.dead=1;dm_android_world_overlay_tick(&state);hidden();hud.dead=0;sample();
    mode=1;out=sample();CHECK(out.word[7]==1&&out.word[16]==700000&&out.word[20]);mini_have=0;out=sample();CHECK(out.word[1]==1);mini_have=1;mini[2]=.5;out=sample();CHECK(out.word[1]==1);mini[2]=.95f;
    mini[0]=NAN;mode=2;out=sample();CHECK(out.word[1]==3&&!out.word[20]&&!out.word[16]);mode=1;out=sample();CHECK(out.word[1]==1);mini[0]=.7f;
    cell=NAN;out=sample();CHECK(out.word[1]==1);cell=4;goals.n=13;out=sample();CHECK(out.word[1]==1);goals.n=1;goals.art[0]=255;out=sample();CHECK(out.word[1]==1);
    for(int art=64;art<=70;art++){goals.art[0]=art;out=sample();CHECK(out.word[1]==3&&out.word[34]==art);}goals.art[0]=30;out=sample();CHECK(out.word[1]==3);
    goals.n=0;out=sample();CHECK(out.word[1]==1);rift_on=0;dm_android_world_overlay_tick(&state);hidden();goals.n=1;out=sample();CHECK(out.word[1]==2&&!out.text[0][0]);
    printf("ANDROID_D2R_WORLD_OVERLAY_OK checks=%d copy_only=1 original_i18n=1 rift_goals=6 countdown=1 scene_lifecycle_timeout=1 map_symbols=7 panel_hide=1 original_game_not_claimed=1\n",checks);return 0;
}
