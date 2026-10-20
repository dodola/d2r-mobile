/* Real shipped library, checked widget transport, shared Custom-page policy.
 * Original layout callbacks are recorded by ARM64 trampolines in a synthetic
 * module; this fixture does not execute the original game's UI implementation. */
#include "../../host/dm.h"
#include "../../host/hle.h"
#include "../../host/dm_widget.h"
#include "../../host/dm_input.h"
#include "../input_backend.h"
#include "../touch_controls_android.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int checks,layouts;
#define CHECK(x) do{++checks;if(!(x)){fprintf(stderr,"Custom fixture failed line=%d\n",__LINE__);return 1;}}while(0)
static void put64(void *w,size_t off,uint64_t value){memcpy((unsigned char *)w+off,&value,8);}
static uint8_t *widgets[80];static unsigned used;static uint64_t base;
static uint8_t *widget(const char *name,uint64_t vt,uint8_t *parent,int x,int y,int width,int height) {
    assert(used<80);uint8_t *w=calloc(1,0xb00);assert(w);widgets[used++]=w;
    put64(w,0,base+vt);put64(w,8,(uintptr_t)name);put64(w,0x30,(uintptr_t)parent);w[0x50]=w[0x51]=1;
    int32_t rect[4]={x,y,width,height};memcpy(w+0x70,rect,16);return w;
}
static void layout_record(uint64_t w){assert(w);layouts++;}
static void trampoline(uint8_t *memory,size_t offset) {
    uint32_t instructions[2]={0x58000050,0xd61f0200};uint64_t fn=(uintptr_t)layout_record;
    memcpy(memory+offset,instructions,8);memcpy(memory+offset+8,&fn,8);
    __builtin___clear_cache((char *)memory+offset,(char *)memory+offset+16);
    uintptr_t page=((uintptr_t)memory+offset)&~4095ull;assert(!mprotect((void *)page,4096,PROT_READ|PROT_EXEC));
}
static void (*scope)(int);static void (*pump)(unsigned long long);static uint8_t *root;
static void run(void){scope(1);pump((uintptr_t)root);scope(0);}
static void *wrong_thread(void *unused){(void)unused;run();return NULL;}
static int checked(uint8_t *w){return w[0xa88]!=0;}
static int frames(uint8_t *w,int on) {
    const int32_t values[2][4]={{0,1,3,2},{4,5,6,-1}};int32_t current;
    memcpy(&current,w+0x610,4);return w[0xa88]==on&&current==values[on][0]&&!memcmp(w+0x6e8,values[on],16);
}
static int ready(void *unused){(void)unused;return 1;}
int main(int argc,char **argv) {
    assert(argc==2);void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);if(!lib){fprintf(stderr,"dlopen %s\n",dlerror());return 1;}
    module_t *mods=dlsym(lib,"g_mod");int *nmods=dlsym(lib,"g_nmod");
    int (*attach)(module_t *)=dlsym(lib,"dm_android_custom_attach");
    void (*gate)(uint64_t,const char *,int,uint64_t)=dlsym(lib,"gate_register");
    void (*init)(const char *)=dlsym(lib,"dm_custom_init");
    int (*get)(const char *)=dlsym(lib,"dm_custom_get");
    int (*fps_setting)(void)=dlsym(lib,"dm_android_d2r_fps_setting");
    int (*quality_mode)(void)=dlsym(lib,"dm_3d_quality_mode_get");
    void (*message)(const char *,const char *,const char *)=dlsym(lib,"dm_custom_on_uimsg");
    void (*generation)(int)=dlsym(lib,"dm_ui_gen_bump");
    void (*pad_boot)(void)=dlsym(lib,"dm_padmode_boot");
    int (*pad_current)(void)=dlsym(lib,"dm_padmode_current");
    uint64_t (*page_filter)(uint64_t)=dlsym(lib,"dm_padmode_page_filter");
    void (*tick)(void)=dlsym(lib,"dm_guest_frame_tick");
    _Atomic int *language=dlsym(lib,"g_dm_lang");
    scope=dlsym(lib,"dm_android_custom_scope");pump=dlsym(lib,"dm_custom_ui_pump");
    int (*input_bind)(const dm_android_input_config *)=dlsym(lib,"dm_android_input_bind");
    void (*touch_resize)(float,float,float,float)=dlsym(lib,"dm_android_touch_resize");
    void (*touch_enable)(int)=dlsym(lib,"dm_android_touch_set_enabled");
    void (*touch_snapshot)(dm_android_touch_snapshot *)=dlsym(lib,"dm_android_touch_snapshot_get");
    int (*touch_event)(const int32_t *,const float *,const float *,int,int)=dlsym(lib,"dm_android_touch_event");
    assert(mods&&nmods&&attach&&gate&&init&&get&&message&&generation&&language&&scope&&pump&&pad_boot&&pad_current&&page_filter&&tick&&fps_setting&&quality_mode&&input_bind&&touch_resize&&touch_enable&&touch_snapshot&&touch_event);
    char dir[]="/data/local/tmp/d2r-custom-fixture-XXXXXX";assert(mkdtemp(dir));init(dir);atomic_store(language,8);
    *nmods=1;strcpy(mods[0].name,"main");mods[0].size=0x3220000;
    mods[0].base=mmap(NULL,mods[0].size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(mods[0].base!=MAP_FAILED);base=(uintptr_t)mods[0].base;
    root=widget("root",DM_VT_BUTTON,NULL,0,0,1280,720);
    uint8_t *panel=widget("VideoOptionsPanel",0x20c0000,root,0,0,1500,1500);
    uint8_t *modal=widget("ConfirmationModal",DM_VT_BUTTON,root,0,0,300,200);modal[0x50]=0;
    uint8_t *scroll=widget("ScrollView",DM_VT_BUTTON,panel,0,0,1500,1260);
    uint8_t *view=widget("view",DM_VT_BUTTON,scroll,0,0,1500,3200);
    uint8_t *rows=widget("TouchCustomRows",DM_VT_BUTTON,view,0,0,1500,3200);
    uint64_t top[2]={(uintptr_t)panel,(uintptr_t)modal},panel_children[1]={(uintptr_t)scroll},scroll_children[1]={(uintptr_t)view},view_children[1]={(uintptr_t)rows},row_children[64];
    put64(root,0x58,(uintptr_t)top);put64(root,0x60,2);put64(panel,0x58,(uintptr_t)panel_children);put64(panel,0x60,1);
    put64(scroll,0x58,(uintptr_t)scroll_children);put64(scroll,0x60,1);put64(view,0x58,(uintptr_t)view_children);put64(view,0x60,1);
    put64(mods[0].base,0x2f05458,(uintptr_t)root);put64(mods[0].base,0x20c0000+7*8,base+0xae5f28);
    const char *keys[]={"lang","auto_mana","merc_pot","drop_light","map_mark","item_notes","icloud","pot_pick","drop_haptic","gem_pick","store_btn","eco_fps","q3d","loot_filter","auto_buff","touch_swap_above"};
    char names[32][64];uint8_t *box[16],*labels[16];int32_t row_y[16];unsigned count=0,visual=0;
    for(int i=0;i<16;i++) {
        row_y[i]=visual*180;
        snprintf(names[i*2],64,"TouchCustom_%s",i?keys[i]:"lang_en");snprintf(names[i*2+1],64,"TouchCustom_%s_Text",keys[i]);
        box[i]=widget(names[i*2],DM_VT_TOGGLE,rows,1315,row_y[i]+30,70,70);box[i][0xa88]=127;
        labels[i]=widget(names[i*2+1],DM_VT_TEXT,rows,25,row_y[i],1000,180);
        row_children[count++]=(uintptr_t)box[i];row_children[count++]=(uintptr_t)labels[i];
        if(i!=6)visual+=(i==12||i==15)?2:1;
    }
    uint8_t *zh=widget("TouchCustom_lang_zh",DM_VT_TOGGLE,rows,905,30,70,70);
    uint8_t *es=widget("TouchCustom_lang_es",DM_VT_TOGGLE,rows,1020,30,70,70);
    uint8_t *ru=widget("TouchCustom_lang_ru",DM_VT_TOGGLE,rows,1315,30,70,70);
    row_children[count++]=(uintptr_t)es;row_children[count++]=(uintptr_t)ru;
    uint8_t *smooth=widget("TouchCustom_q3d_smooth",DM_VT_TOGGLE,rows,1315,row_y[12]+180,70,70);
    uint8_t *near=widget("TouchCustom_touch_swap_above_near",DM_VT_TOGGLE,rows,1315,row_y[15]+180,70,70);
    uint8_t *hint_en=widget("TouchCustomLangHintEn",DM_VT_TEXT,rows,99999,112,1400,60);
    uint8_t *hint_zh=widget("TouchCustomLangHintZh",DM_VT_TEXT,rows,99999,112,1400,60);
    uint8_t *selection=widget("TouchCustomSel",0x20bc048,rows,99999,0,1000,100);
    row_children[count++]=(uintptr_t)zh;row_children[count++]=(uintptr_t)smooth;row_children[count++]=(uintptr_t)near;
    row_children[count++]=(uintptr_t)hint_en;row_children[count++]=(uintptr_t)hint_zh;row_children[count++]=(uintptr_t)selection;
    put64(rows,0x58,(uintptr_t)row_children);put64(rows,0x60,count);
    put64(mods[0].base,DM_VT_TEXT+0x10,base+0xb79f50);put64(mods[0].base,0x20bc048+0x10,base+0xb0a9ac);
    trampoline(mods[0].base,0xb79f50);trampoline(mods[0].base,0xb0a9ac);
    CHECK(!attach(&mods[0]));strcpy(mods[0].build_id,"db0507f36b84200ac05f75d76ed5026f");CHECK(!attach(&mods[0]));
    gate(0x1000,"obs",OBS_UIMSG,0);gate(0x2000,"obs",OBS_TICK,0);gate(0x3000,"obs",OBS_C2S,0);CHECK(!attach(&mods[0]));
    gate(0x8100,"svc",SVC_HOST_C2S_DELIVER,0);size_t size=mods[0].size;mods[0].size=0x1000;CHECK(!attach(&mods[0]));mods[0].size=size;CHECK(attach(&mods[0]));
    pump((uintptr_t)root);CHECK(box[1][0xa88]==127);run();
    CHECK(box[6][0xa88]==127);
    for(int i=0;i<16;i++){if(i==6)continue;int def=i!=0&&i!=11&&i!=12&&i!=15;CHECK(frames(box[i],def)&&get(keys[i])==def);}
    CHECK(frames(zh,1)&&frames(smooth,1)&&frames(near,1)&&!panel[0x1a0]);
    CHECK(fps_setting()==60);box[11][0xa88]=1;run();CHECK(panel[0x1a0]&&fps_setting()==60);
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(fps_setting()==60&&!checked(box[11]));
    box[11][0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(fps_setting()==30);
    box[11][0xa88]=0;run();CHECK(fps_setting()==30);message("OptionsPanelMessage","CancelChanges","");run();CHECK(fps_setting()==30&&checked(box[11]));
    box[11][0xa88]=0;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(fps_setting()==60);
    box[1][0xa88]=0;run();CHECK(get("auto_mana")==1&&panel[0x1a0]);
    box[1][0xa88]=1;run();CHECK(!panel[0x1a0]);
    box[1][0xa88]=0;run();message("OptionsPanelMessage","CancelChanges","");run();CHECK(frames(box[1],1)&&get("auto_mana")==1&&!panel[0x1a0]);
    box[1][0xa88]=0;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(!get("auto_mana")&&!panel[0x1a0]);
    char path[1200],temporary[1210],saved[4096]={0};snprintf(path,sizeof path,"%s/sw_custom.txt",dir);snprintf(temporary,sizeof temporary,"%s.tmp",path);
    FILE *f=fopen(path,"r");CHECK(f&&fread(saved,1,sizeof saved-1,f)>0);fclose(f);CHECK(strstr(saved,"auto_mana=0\n")&&!strstr(saved,"lang="));CHECK(access(temporary,F_OK)!=0);
    message("OptionsPanelMessage","ResetToDefaults","");run();CHECK(frames(box[1],1)&&panel[0x1a0]&&!get("auto_mana"));
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(frames(box[1],0)&&!panel[0x1a0]);
    message("OptionsPanelMessage","ResetToDefaults","");run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("auto_mana")&&!panel[0x1a0]);
    CHECK(quality_mode()==0);
    smooth[0xa88]=0;run();CHECK(frames(smooth,1)&&!checked(box[12])&&!panel[0x1a0]&&quality_mode()==0);
    box[12][0xa88]=1;run();CHECK(checked(box[12])&&!checked(smooth)&&panel[0x1a0]&&!get("q3d")&&quality_mode()==0);
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(frames(box[12],0)&&frames(smooth,1)&&!panel[0x1a0]&&quality_mode()==0);
    box[12][0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("q3d")==1&&quality_mode()==1&&!panel[0x1a0]);
    smooth[0xa88]=1;run();CHECK(quality_mode()==1&&get("q3d")==1&&panel[0x1a0]);
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(quality_mode()==1&&checked(box[12]));
    smooth[0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("q3d")==0&&quality_mode()==0&&!panel[0x1a0]);
    near[0xa88]=0;run();CHECK(frames(near,1)&&!checked(box[15])&&!panel[0x1a0]);
    box[15][0xa88]=1;run();CHECK(checked(box[15])&&!checked(near)&&panel[0x1a0]&&!get("touch_swap_above"));
    message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("touch_swap_above")&&!panel[0x1a0]);
    near[0xa88]=1;run();CHECK(!checked(box[15])&&checked(near)&&panel[0x1a0]&&get("touch_swap_above"));
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(frames(box[15],1)&&frames(near,0)&&!panel[0x1a0]);
    /* Exercise the actual Android layout consumer, including cancellation of
     * an existing contact only after committing the changed radio setting. */
    dm_android_input_config input={.source="custom-consumer-fixture",.panel_width=1280,.panel_height=720,.source_ready=ready,.logical_npad_style=1};
    CHECK(!input_bind(&input));touch_resize(1280,720,0,0);touch_enable(1);
    dm_android_touch_snapshot ts;touch_snapshot(&ts);CHECK(ts.ready&&ts.enabled&&ts.controls.layout.swap_above==1);
    int32_t contact_id=71;float contact_x=.15f,contact_y=.5f;CHECK(!touch_event(&contact_id,&contact_x,&contact_y,1,0));
    touch_snapshot(&ts);uint32_t before_cancel=ts.cancel_generation;int contacts=0;for(unsigned i=0;i<DM_TC_MAX_TOUCH;i++)contacts+=ts.controls.finger[i].active!=0;CHECK(contacts==1);
    near[0xa88]=1;run();CHECK(!touch_event(&contact_id,&contact_x,&contact_y,1,0));touch_snapshot(&ts);CHECK(ts.controls.layout.swap_above==1&&ts.cancel_generation==before_cancel);
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(!touch_event(&contact_id,&contact_x,&contact_y,1,0));touch_snapshot(&ts);CHECK(ts.controls.layout.swap_above==1&&ts.cancel_generation==before_cancel);
    near[0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(!touch_event(&contact_id,&contact_x,&contact_y,1,0));touch_snapshot(&ts);CHECK(ts.controls.layout.swap_above==0&&ts.cancel_generation>before_cancel);
    contacts=0;for(unsigned i=0;i<DM_TC_MAX_TOUCH;i++)contacts+=ts.controls.finger[i].active!=0;CHECK(!contacts);
    box[15][0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(!touch_event(NULL,NULL,NULL,0,0));touch_snapshot(&ts);CHECK(ts.controls.layout.swap_above==1);
    message("OptionsPanelMessage","ResetToDefaults","");run();CHECK(!touch_event(NULL,NULL,NULL,0,0));touch_snapshot(&ts);CHECK(ts.controls.layout.swap_above==1);
    message("OptionsPanelMessage","AcceptChanges","");run();CHECK(!touch_event(NULL,NULL,NULL,0,0));touch_snapshot(&ts);CHECK(ts.controls.layout.swap_above==0);
    /* Restore the committed radio state expected by the later page checks. */
    box[15][0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(!touch_event(NULL,NULL,NULL,0,0));
    box[0][0xa88]=1;run();int32_t hint_x;memcpy(&hint_x,hint_en+0x70,4);CHECK(checked(box[0])&&!checked(zh)&&panel[0x1a0]&&hint_x==25&&layouts>0);
    message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("lang")==1&&!panel[0x1a0]);
    es[0xa88]=1;run();CHECK(checked(es)&&!checked(zh)&&!checked(box[0])&&!checked(ru)&&get("lang")==1&&panel[0x1a0]);
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(checked(box[0])&&!checked(es)&&!panel[0x1a0]);
    ru[0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("lang")==3&&checked(ru)&&!panel[0x1a0]);
    es[0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("lang")==2&&checked(es)&&!panel[0x1a0]);
    box[0][0xa88]=1;run();message("OptionsPanelMessage","AcceptChanges","");run();CHECK(get("lang")==1&&checked(box[0])&&!panel[0x1a0]);
    message("OptionsPanelMessage","ResetToDefaults","");run();CHECK(checked(box[0])&&!checked(zh));message("OptionsPanelMessage","CancelChanges","");run();
    box[1][0xa88]=0;run();CHECK(panel[0x1a0]);panel[0x50]=0;run();panel[0x50]=1;run();CHECK(frames(box[1],1)&&get("auto_mana")==1&&!panel[0x1a0]);
    box[1][0xa88]=0;run();message("OptionsPanelMessage","CancelChanges","");run();
    put64(box[1],0,base+DM_VT_BUTTON);box[1][0xa88]=127;run();CHECK(box[1][0xa88]==127&&!panel[0x1a0]);
    put64(box[1],0,base+DM_VT_TOGGLE);run();CHECK(frames(box[1],1)&&!panel[0x1a0]);
    row_children[2]=0;run();box[1][0xa88]=0;row_children[2]=(uintptr_t)box[1];run();CHECK(frames(box[1],1)&&!panel[0x1a0]);
    box[1][0xa88]=0;generation(17);run();CHECK(frames(box[1],1)&&!panel[0x1a0]);
    put64(mods[0].base,0x20c0000+7*8,0);box[1][0xa88]=0;run();CHECK(!panel[0x1a0]&&get("auto_mana")==1);
    message("OptionsPanelMessage","CancelChanges","");run();put64(mods[0].base,0x20c0000+7*8,base+0xae5f28);
    uint64_t original=row_children[2];row_children[2]=1;run();CHECK(!panel[0x1a0]);row_children[2]=original;
    put64(rows,0x60,257);run();CHECK(!panel[0x1a0]);put64(rows,0x60,count);
    row_children[2]=(uintptr_t)panel;run();CHECK(!panel[0x1a0]);row_children[2]=original;
    uint64_t old_root=(uintptr_t)root;put64(mods[0].base,0x2f05458,1);box[1][0xa88]=0;run();CHECK(box[1][0xa88]==0&&!panel[0x1a0]);put64(mods[0].base,0x2f05458,old_root);box[1][0xa88]=1;
    box[1][0xa88]=0;pthread_t thread;CHECK(!pthread_create(&thread,NULL,wrong_thread,NULL)&&!pthread_join(thread,NULL));CHECK(!panel[0x1a0]&&get("auto_mana")==1);box[1][0xa88]=1;
    uint64_t bad_fn=base+0xb79f54;put64(mods[0].base,DM_VT_TEXT+0x10,bad_fn);box[0][0xa88]=0;run();CHECK(get("lang")==1);memcpy(&hint_x,hint_zh+0x70,4);CHECK(hint_x==99999);
    put64(mods[0].base,DM_VT_TEXT+0x10,base+0xb79f50);message("OptionsPanelMessage","CancelChanges","");run();
    /* Use the production controller mode bootstrap and page key ownership,
     * with isolated overlay markers and the original expected prefix bytes. */
    const struct {size_t offset;const char *text;} prefixes[]={
        {0x1d21db5,"Controller/Console/Switch/"},{0x1d2c33f,"Controller/Console/"},{0x1d3b705,"Controller/"},
        {0x1d0f5b8,"Controller/ControllerButtons/%s/%s"},{0x1d076c7,"Controller/Cursor/Idle"},
        {0x1d3acbc,"Controller/Cursor/Hold"},{0x1d4d5ea,"Controller/Cursor/Held"}};
    for(unsigned i=0;i<sizeof prefixes/sizeof *prefixes;i++)strcpy((char *)mods[0].base+prefixes[i].offset,prefixes[i].text);
    snprintf(path,sizeof path,"%s/ctrl_touch",dir);f=fopen(path,"w");CHECK(f);fclose(f);
    snprintf(path,sizeof path,"%s/touch_strings.tsv",dir);f=fopen(path,"w");CHECK(f);fclose(f);
    CHECK(!setenv("DM_ROMFS_OVERLAY",dir,1)&&!setenv("DM_APP","1",1));pad_boot();CHECK(pad_current()==1);
    run();int32_t selected_rect[4];memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[0]==-5&&selected_rect[1]==-10);
    CHECK(!page_filter(DM_BTN_DOWN));run();memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[1]==170);
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_UP));run();memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[1]==-10);
    for(int i=1;i<=5;i++){CHECK(!page_filter(0)&&!page_filter(DM_BTN_DOWN));run();}
    memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[1]==row_y[5]-10);
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_DOWN));run();memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[1]==row_y[7]-10&&box[6][0xa88]==127);
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_UP));run();memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[1]==row_y[5]-10&&box[6][0xa88]==127);
    for(int i=1;i<=5;i++){CHECK(!page_filter(0)&&!page_filter(DM_BTN_UP));run();}
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_DOWN));run();
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_A));run();CHECK(!checked(box[1])&&get("auto_mana")==1&&panel[0x1a0]);
    CHECK(!page_filter(DM_BTN_A));run();CHECK(!checked(box[1])&&panel[0x1a0]);
    message("OptionsPanelMessage","AcceptChanges","");run();CHECK(!get("auto_mana")&&!panel[0x1a0]);
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_A));run();CHECK(checked(box[1])&&!get("auto_mana")&&panel[0x1a0]);
    message("OptionsPanelMessage","CancelChanges","");run();CHECK(!checked(box[1])&&!panel[0x1a0]);
    modal[0x50]=1;run();memcpy(selected_rect,selection+0x70,16);CHECK(selected_rect[0]==99999);
    for(int i=0;i<4;i++)tick();CHECK(!page_filter(0)&&page_filter(DM_BTN_A)==DM_BTN_A);run();CHECK(!checked(box[1])&&!panel[0x1a0]);
    modal[0x50]=0;run();CHECK(!page_filter(0)&&!page_filter(DM_BTN_A));run();CHECK(checked(box[1])&&panel[0x1a0]);
    message("OptionsPanelMessage","CancelChanges","");run();
    /* 设置入口只排队：保留待应用值，确认框与错误控件类型不得触发。
     * The settings entry only queues a request: preserve pending values and reject modal/wrong-class activation. */
    int (*take_request)(void)=dlsym(lib,"dm_android_custom_take_request");
    void (*platform_message)(const char *,const char *,const char *)=dlsym(lib,"dm_android_custom_message");
    CHECK(take_request&&platform_message&&!take_request());
    box[1][0xa88]=1;run();CHECK(panel[0x1a0]&&!get("auto_mana"));
    uint8_t *settings=widget("TouchAndroidSettings",DM_VT_BUTTON,rows,470,15,470,128);
    row_children[count++]=(uintptr_t)settings;put64(rows,0x60,count);generation(18);run();
    CHECK(!page_filter(0)&&!page_filter(DM_BTN_A));run();CHECK(take_request()==1&&!take_request()&&panel[0x1a0]&&!get("auto_mana"));
    CHECK(!page_filter(DM_BTN_A));run();CHECK(!take_request());
    modal[0x50]=1;run();for(int i=0;i<4;i++)tick();CHECK(!page_filter(0)&&page_filter(DM_BTN_A)==DM_BTN_A);run();CHECK(!take_request());modal[0x50]=0;
    put64(settings,0,base+DM_VT_TOGGLE);run();CHECK(!page_filter(0)&&!page_filter(DM_BTN_A));run();CHECK(!take_request());
    put64(settings,0,base+DM_VT_BUTTON);
    platform_message("PanelManager","OpenPanel","TouchAndroidSettings");CHECK(!take_request());
    platform_message("PanelManager","ClosePanel","TouchAndroidSettings");CHECK(take_request()==1&&!take_request());
    printf("ANDROID_D2R_CUSTOM_OK checks=%d shared_rows=16 android_navigable=15 checked_widget_transport=1 original_layout_call_recorded=1 apply_discard_defaults=1 radio_pairs=3 controller_page_keys=1 controller_repeat_guard=1 modal_release=1 dirty_original_contract=1 disk_commit=1 lifetime_guards=1 guest_thread_scope=1 platform_settings_entry=1 synthetic_memory=1 original_game_not_claimed=1\n",checks);
    return 0;
}
