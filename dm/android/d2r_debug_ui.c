/* Chronicle Debug tab. All widgets are original game widgets; this module
 * runs on the client UI owner. Mutations cross to the server as commands. */
#include "d2r_debug_mode.h"
#include "d2r_map_reveal.h"
#include "../host/dm_widget.h"
#include "../host/dm_cursor.h"
#include "../host/dm_uiscreen.h"
#include "../host/hle.h"
#include "dm_vault_tab.h"
#include <stdatomic.h>
#include "d2r_debug_scenes.inc"

static _Atomic int opened,open_request;
static _Atomic float entry_aspect;
static _Atomic int entry_ready;
static int category,filter,page,selected=-1,catalog_dirty=1;
static char notice[256];
enum { ROWS=6,CATEGORIES=9,FILTERS=5,CAT_SCENES=7,CAT_TOOLS=8 };
static struct { int kind,index; char code[5],name[192]; } entries[2048];
static int count;
int dm_android_debug_ui_is_open(void){return atomic_load(&opened);}
int dm_android_debug_ui_touch(int phase,float x,float y) {
    static int captured,moved;static float down_x,down_y;
    if(phase==3){int had=captured;captured=0;moved=0;return had;}
    if(atomic_load(&opened)){dm_ui_touch(phase,x,y);return 1;}
    float aspect=atomic_load(&entry_aspect);
    int hit=atomic_load(&entry_ready)&&aspect>0&&x>=80.f/(2160*aspect)&&x<=520.f/(2160*aspect)&&y>=390.f/2160&&y<=550.f/2160;
    if(phase==0){captured=hit;moved=0;down_x=x;down_y=y;return hit;}
    if(!captured)return 0;
    if((x-down_x)*(x-down_x)+(y-down_y)*(y-down_y)>0.0004f)moved=1;
    if(phase==2){captured=0;if(hit&&!moved)atomic_store(&open_request,1);}
    return 1;
}
static uint64_t find(uint64_t w,const char *name,int depth) {
    if(!w || depth>12)return 0;
    const char *n=*(const char **)(w+8);if(n && !strcmp(n,name))return w;
    uint64_t *children=*(uint64_t **)(w+0x58),nchild=*(uint64_t *)(w+0x60);
    if(!children || nchild>256)return 0;
    for(uint64_t i=0;i<nchild;i++){uint64_t r=find(children[i],name,depth+1);if(r)return r;}return 0;
}
static void move(uint64_t w,int x) {
    if(!w || *(int32_t *)(w+0x70)==x)return;
    *(int32_t *)(w+0x70)=x;((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)w+0x10))(w);
}
static void text(uint8_t *b,uint64_t body,const char *name,const char *s) {
    uint64_t w=find(body,name,0);if(!dm_widget_is(b,w,DM_VT_TEXT))return;
    size_t n=strlen(s);if(*(uint64_t *)(w+0x90)==n && !memcmp(*(const char **)(w+0x88),s,n))return;
    ((void *(*)(uint64_t,const char *,uint64_t))(b+0x136e5c))(w+0x88,s,n);
    ((void (*)(uint64_t))(b+0xadc434))(w);
}
static const char *localized(uint8_t *b,const char *key) {
    struct {const char *p;uint64_t n;} s={key,strlen(key)};
    const char *r=((const char *(*)(void *))(b+0x7f5630))(&s);return r&&*r?r:key;
}
static void append(int kind,int index,const char *code,const char *name) {
    if(count==(int)(sizeof entries/sizeof entries[0]))return;
    entries[count].kind=kind;entries[count].index=index;
    snprintf(entries[count].code,sizeof entries[count].code,"%.4s",code?code:"");
    /* Annotations are multi-line; show the final (item-name) line. */
    const char *last=strrchr(name,'\n');if(last)name=last+1;
    snprintf(entries[count].name,sizeof entries[count].name,"%s",name);count++;
}
/* Item table sections follow the source BIN files: Misc 169, Weapons 306,
 * Armor 217 rows, each file contiguous (first rows elx / hax / cap). */
enum { SEC_ALL, SEC_WEAPON, SEC_ARMOR, SEC_MISC, SEC_NONE };
static int table_find(const uint8_t *rows,uint32_t n,const char *code) {
    uint32_t want=0x20202020u;memcpy(&want,code,strlen(code));
    for(uint32_t i=0;i<n;i++){uint32_t v;memcpy(&v,rows+(size_t)i*0x1c0+0x80,4);if(v==want)return (int)i;}
    return -1;
}
static int section_of(const uint8_t *rows,uint32_t n,uint32_t row) {
    static const uint32_t sizes[3]={169,306,217};
    static const char *const firsts[3]={"elx","hax","cap"};
    static const int kinds[3]={SEC_MISC,SEC_WEAPON,SEC_ARMOR};
    for(int k=0;k<3;k++){int start=table_find(rows,n,firsts[k]);if(start>=0&&row>=(uint32_t)start&&row<(uint32_t)start+sizes[k])return kinds[k];}
    return SEC_NONE;
}
static int is_rune_code(const char *c) {
    return strlen(c)==3&&c[0]=='r'&&c[1]>='0'&&c[1]<='3'&&c[2]>='0'&&c[2]<='9'&&atoi(c+1)>=1&&atoi(c+1)<=33;
}
static int is_gem_code(const char *c) {
    if(strlen(c)!=3)return 0;
    if(c[0]=='g')return strchr("cfslpz",c[1])&&strchr("vybgrw",c[2]);
    return c[0]=='s'&&c[1]=='k'&&strchr("cfulz",c[2]);
}
static void catalog(uint8_t *b) {
    count=0;
    uint8_t version=*(uint8_t *)(b+0x2127c48);if(version<1||version>3)return;
    uint64_t table=*(uint64_t *)(b+0x2d64a18+version*16);if(!table)return;
    uint8_t *rows=*(uint8_t **)(table+0x15c0);uint32_t n=*(uint32_t *)(table+0x15c8);if(!rows||n>16384)return;
    if(category<2){
        int kind=category==0?1:0;
        uint8_t *list=*(uint8_t **)(table+(kind?0x13e8:0x13c8));uint32_t listed=*(uint32_t *)(table+(kind?0x13f0:0x13d0));
        size_t stride=kind?0x15c:0x1cc;
        for(int i=0;i<VT_NROW;i++){
            if(vt_rows[i].kind!=kind)continue;
            if(filter){
                uint32_t code=0;int sec=SEC_NONE;
                if(list&&(uint32_t)vt_rows[i].row<listed&&listed<=2048){
                    memcpy(&code,list+(size_t)vt_rows[i].row*stride+0x28,4);
                    for(uint32_t r=0;r<n;r++){uint32_t v;memcpy(&v,rows+(size_t)r*0x1c0+0x80,4);if(v==code){sec=section_of(rows,n,r);break;}}
                }
                if(sec!=filter)continue;
            }
            append(vt_rows[i].kind,vt_rows[i].row,NULL,localized(b,vt_rows[i].key));
        }
        return;
    }
    if(category>=2&&category<=6){
        for(uint32_t i=0;i<n;i++){
            char code[5];memcpy(code,rows+(size_t)i*0x1c0+0x80,4);code[4]=0;
            for(int j=3;j>=0&&(code[j]==' '||!code[j]);j--)code[j]=0;
            if(!*code || strspn(code,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")!=strlen(code))continue;
            int sec=section_of(rows,n,i),rune=is_rune_code(code),gem=is_gem_code(code);
            int keep=category==2?rune:category==3?gem:category==4?sec==SEC_WEAPON:category==5?sec==SEC_ARMOR:sec==SEC_MISC&&!rune&&!gem;
            if(keep)append(-1,0,code,localized(b,code));
        }
        return;
    }
    if(category==CAT_SCENES){
        static const int act_first[6]={1,40,75,103,109,137};
        for(unsigned i=0;i<sizeof debug_scenes/sizeof *debug_scenes;i++){
            int id=debug_scenes[i].id;
            if(id<act_first[filter]||id>=act_first[filter+1])continue;
            char name[192];snprintf(name,sizeof name,"%s  [%d]",debug_scenes[i].name,id);
            append(-2,id,NULL,name);
        }
    }
}
static int make_selected(void) {
    if(selected<0||selected>=count||!dm_android_debug_active())return 0;
    char cmd[64];
    if(entries[selected].kind==-2){snprintf(cmd,sizeof cmd,"scene %d",entries[selected].index);return dm_android_debug_submit(cmd);}
    /* Everything is dropped on the ground beside the character. */
    if(entries[selected].kind==-1){snprintf(cmd,sizeof cmd,"give %s 2 1",entries[selected].code);return dm_android_debug_submit(cmd);}
    snprintf(cmd,sizeof cmd,"catalog %d %d",entries[selected].kind,entries[selected].index);
    LOG("ANDROID_DEBUG_UI command=%s",cmd);
    return dm_android_debug_submit(cmd);
}
/* Message handling records intent. Widget writes are deferred to the UI pump,
 * never performed by an arbitrary observer caller. */
static int action;
void dm_android_debug_ui_message(const char *cat,const char *act,const char *arg) {
    if(!strcmp(cat,"PanelManager")&&!strcmp(act,"ClosePanel")&&!strcmp(arg,"TouchDBG_Open")) {
        if(dm_android_debug_active())open_request=1;return;
    }
    if(!strcmp(cat,"PanelManager")&&(!strcmp(act,"UnloadAll")||
        ((!strcmp(act,"ClosePanel")||!strcmp(act,"UnloadPanel"))&&!strcmp(arg,"ChroniclePanel")))){opened=0;action=0;return;}
    if(!opened||strcmp(cat,"PanelManager")||strcmp(act,"ClosePanel")||strncmp(arg,"TouchDBG_",9))return;
    const char *p=arg+9;
    if(!strncmp(p,"Category",8)&&p[8]>='0'&&p[8]<'0'+CATEGORIES&&!p[9]){category=p[8]-'0';filter=0;page=0;selected=-1;catalog_dirty=1;notice[0]=0;}
    else if(!strncmp(p,"Filter",6)&&p[6]>='0'&&p[6]<'0'+FILTERS&&!p[7]){if(category<2||category==CAT_SCENES){filter=p[6]-'0';page=0;selected=-1;catalog_dirty=1;}}
    else if(!strcmp(p,"Prev")){if(page>0)page--;selected=-1;}
    else if(!strcmp(p,"Next")){if((page+1)*ROWS<count)page++;selected=-1;}
    else if(!strncmp(p,"Row",3)&&p[3]>='0'&&p[3]<'0'+ROWS&&!p[4])selected=page*ROWS+p[3]-'0';
    else if(!strcmp(p,"Add"))action=1;
    else if(!strcmp(p,"Reveal"))action=4;
    else if(!strcmp(p,"Heal"))action=5;
    else if(!strcmp(p,"Gold"))action=6;
    else if(!strcmp(p,"Skill"))action=7;
    else if(!strcmp(p,"Stat"))action=8;
}
static void show(uint64_t w,int on) {
    if(w){*(uint8_t *)(w+0x50)=on?1:0;*(uint8_t *)(w+0x51)=on?1:0;}
}
/* ImageButton dimensions are replaced by sprite dimensions at load. Set native
 * button rectangles to their explicit parent hit areas once loaded. */
static void fit_buttons(uint8_t *b,uint64_t w,int depth) {
    if(!w||depth>4)return;
    uint64_t *c=*(uint64_t **)(w+0x58),nc=*(uint64_t *)(w+0x60);
    if(!c||nc>256)return;
    const char *name=*(const char **)(w+8);
    if(name&&!strncmp(name,"TouchDBG_",9)&&strstr(name,"Box")){
        for(uint64_t i=0;i<nc;i++)if(dm_widget_is(b,c[i],DM_VT_BUTTON)){
            int32_t *r=(int32_t *)(c[i]+0x70),*pr=(int32_t *)(w+0x70);
            if(r[2]!=pr[2]||r[3]!=pr[3]){r[2]=pr[2];r[3]=pr[3];((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)c[i]+0x10))(c[i]);}
        }
        return;
    }
    for(uint64_t i=0;i<nc;i++)fit_buttons(b,c[i],depth+1);
}
void dm_android_debug_ui_pump(uint8_t *b) {
    if(!b)return;
    int32_t width=*(int32_t *)(b+0x211da38),height=*(int32_t *)(b+0x211da3c);
    if(width>0&&height>0)atomic_store(&entry_aspect,(float)width/height);
    atomic_store(&entry_ready,dm_android_debug_active()&&dm_uiscreen_ingame()&&!dm_ui_panel_state());
    uint64_t root=*(uint64_t *)(b+0x2f05458);
    uint64_t hp=dm_ui_top(root,"HelpButtonPanel");
    uint64_t entry=find(hp,"TouchDBG_Entry",0);
    /* Original Help is hidden in controller mode. Its replacement is a native
     * touch entry and follows our in-game state instead of the Help policy. */
    if(hp&&entry&&dm_android_debug_active()&&dm_uiscreen_ingame()){
        *(uint8_t *)(hp+0x50)=1;*(uint8_t *)(hp+0x51)=1;
    }
    static uint64_t entry_logged;
    if(entry!=entry_logged){entry_logged=entry;LOG("ANDROID_DEBUG_UI entry=%llx parent=%llx visible=%d rect=%d,%d",(unsigned long long)entry,(unsigned long long)hp,hp?*(uint8_t *)(hp+0x50):0,hp?*(int32_t *)(hp+0x70):0,hp?*(int32_t *)(hp+0x74):0);}
    move(entry,dm_android_debug_active()?0:99999);
    uint64_t eb=find(entry,"TouchDBG_Open",0);
    if(dm_widget_is(b,eb,DM_VT_BUTTON)&&(*(int32_t *)(eb+0x78)!=440||*(int32_t *)(eb+0x7c)!=160)){
        *(int32_t *)(eb+0x78)=440;*(int32_t *)(eb+0x7c)=160;
        ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)eb+0x10))(eb);
    }
    void dm_ui_send(const char *,const char *,const char *);
    if(atomic_exchange(&open_request,0)){opened=1;catalog_dirty=1;notice[0]=0;
        dm_ui_send("PanelManager","OpenPanel","ChroniclePanel");
        LOG("ANDROID_DEBUG_UI standalone=1 open=1");
    }
    uint64_t cp=dm_ui_top(root,"ChroniclePanel");
    uint64_t body=find(cp,"TouchDBG_Body",0);
    if(!body){action=0;return;}
    /* Keep the registered shell, hide its ordinary content while Debug owns it.
     * Saved visibility belongs to this widget generation, never a stale tree. */
    static uint64_t owner;static uint32_t generation;static uint8_t visible[256];static int hidden;
    uint32_t gen=dm_ui_gen();
    if(owner!=cp||generation!=gen){owner=cp;generation=gen;hidden=0;}
    uint64_t *ch=*(uint64_t **)(cp+0x58),n=*(uint64_t *)(cp+0x60);
    if(n>256)return;
    if(opened&&!hidden){
        for(uint64_t i=0;i<n;i++){visible[i]=*(uint8_t *)(ch[i]+0x50);if(ch[i]!=body)*(uint8_t *)(ch[i]+0x50)=0;}
        hidden=1;
        const char *panels[]={"ChronicleUniquesListPanel","ChronicleSetsListPanel","ChronicleRunewordsListPanel"};
        for(unsigned i=0;i<3;i++)dm_ui_send("PanelManager","ClosePanel",panels[i]);
    }else if(!opened&&hidden){for(uint64_t i=0;i<n;i++)if(ch[i]!=body)*(uint8_t *)(ch[i]+0x50)=visible[i];hidden=0;}
    move(body,opened?0:99999);if(!opened)return;
    uint64_t workspace=find(body,"Workspace",0);
    /* The workspace anchor is not resolved for this generated panel (its parent
     * is sized after creation), so centre the 3060x2040 canvas explicitly in
     * the 2160-high game space of the current display aspect. */
    float aspect=atomic_load(&entry_aspect);
    if(workspace&&aspect>0){
        int32_t wx=(int32_t)((2160.f*aspect-3060.f)/2.f),wy=(2160-2040)/2;
        int32_t *pos=(int32_t *)(workspace+0x70);
        if(pos[0]!=wx||pos[1]!=wy){pos[0]=wx;pos[1]=wy;
            ((void (*)(uint64_t))*(uint64_t *)(*(uint64_t *)workspace+0x10))(workspace);
            LOG("ANDROID_DEBUG_UI workspace=%d,%d aspect=%.3f",wx,wy,aspect);}
    }
    fit_buttons(b,workspace,0);
    int tools=category==CAT_TOOLS;
    move(find(body,"TouchDBG_ListGroup",0),tools?99999:0);
    move(find(body,"TouchDBG_ToolsGroup",0),tools?0:99999);
    for(int i=0;i<CATEGORIES;i++){char name[40];snprintf(name,sizeof name,"TouchDBG_Category%dHi",i);show(find(body,name,0),i==category);}
    {
        static const char *const gear[FILTERS]={"全部","武器","护甲","饰品",""};
        static const char *const acts[FILTERS]={"第一幕","第二幕","第三幕","第四幕","第五幕"};
        int chips=category<2?4:category==CAT_SCENES?5:0;
        for(int i=0;i<FILTERS;i++){
            char name[40];snprintf(name,sizeof name,"TouchDBG_Filter%dBox",i);move(find(body,name,0),i<chips?710+i*460:99999);
            snprintf(name,sizeof name,"TouchDBG_Filter%dHi",i);show(find(body,name,0),i<chips&&i==filter);
            snprintf(name,sizeof name,"TouchDBG_Filter%dText",i);text(b,body,name,category==CAT_SCENES?acts[i]:gear[i]);
        }
    }
    if(catalog_dirty){catalog(b);catalog_dirty=0;LOG("ANDROID_DEBUG_UI category=%d entries=%d",category,count);}
    if(page*ROWS>=count)page=0;
    if(action){int a=action;action=0;int ok=0;
        if(a==1)ok=make_selected();
        if(a==4)ok=dm_android_map_request_when_ready(NULL)==0;
        if(a==5)ok=dm_android_debug_submit("heal");
        if(a==6)ok=dm_android_debug_submit("gold add 100000");
        if(a==7)ok=dm_android_debug_submit("points skill 10");
        if(a==8)ok=dm_android_debug_submit("points stat 100");
        snprintf(notice,sizeof notice,"%s",!ok?"未执行：请选择条目，或等待当前操作完成":a==1&&category==CAT_SCENES?"已提交：正在前往":a==1?"已提交：物品会掉落在角色脚边":"已提交");
        LOG("ANDROID_DEBUG_UI action=%d accepted=%d",a,ok);
    }
    char label[256];
    for(int i=0;i<ROWS;i++){
        int ix=page*ROWS+i;char name[40];
        snprintf(name,sizeof name,"TouchDBG_Row%dHi",i);show(find(body,name,0),ix<count&&ix==selected);
        const char *kind[3]={"","U","S"};
        for(int k=0;k<3;k++){
            snprintf(name,sizeof name,"TouchDBG_RowText%s%d",kind[k],i);
            int mine=k==(category==0?1:category==1?2:0);
            snprintf(label,sizeof label,"%s",ix<count&&mine?entries[ix].name:"");
            text(b,body,name,label);
        }
    }
    snprintf(label,sizeof label,"第 %d / %d 页 · 共 %d 项",page+1,count?(count+ROWS-1)/ROWS:1,count);text(b,body,"TouchDBG_Page",label);
    text(b,body,"TouchDBG_AddText",category==CAT_SCENES?"前往选中场景":"放到地面");
    text(b,body,"TouchDBG_Selection",selected>=0&&selected<count?entries[selected].name:"请先选择分类和条目");
    dm_android_debug_summary(label,sizeof label);label[110]=0;text(b,body,"TouchDBG_Status",label);
    text(b,body,"TouchDBG_Notice",notice);
}
