/* Checked transport for the unchanged shared Custom-page settings policy. */
#include "d2r_custom_android.h"
#include "../host/dm_custom_ui.h"
#include "../host/hle.h"
#include "../host/dm_cursor.h"
#include "../host/dm_widget.h"
#include "../host/dm_custom.h"
#include "../ios/Sources/dm_touch_game_identity.h"
#include <sys/uio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdatomic.h>

enum { NODE_MAX=2048, ROOT_OFF=0x2f05458, NINE_TILE_VT=0x20bc048 };
typedef struct {
    uint64_t w,vt,name,children,count;
    unsigned parent,index,depth;
    char text[64];
} node;
typedef struct {node nodes[NODE_MAX];unsigned count;uint64_t root;uint32_t generation;} context;
static module_t *title;
static pthread_mutex_t owner_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t owner;static int have_owner;
static _Thread_local int scoped;
static _Thread_local context current;
#include "d2r_custom_settings.inc"

static int read_vm(uint64_t p,void *out,size_t bytes) {return dm_tg_vm_read(p,out,bytes);}
static int title_ok(void) {
    return title&&g_nmod>0&&title==&g_mod[0]&&title->base&&title->size>=0x3211700&&
        !strcmp(title->name,"main")&&!strcmp(title->build_id,"db0507f36b84200ac05f75d76ed5026f");
}
int dm_android_title_custom_settings(void) {return title_ok();}
void dm_android_custom_message(const char *category,const char *action,const char *argument) {
    if(!title_ok()||!category||!action||!argument||strcmp(category,"PanelManager")||strcmp(action,"ClosePanel"))return;
    if(!strcmp(argument,"TouchCustom_mod"))atomic_fetch_or(&action_clicks,1);
    if(!strcmp(argument,"TouchCustom_map_reveal"))atomic_fetch_or(&action_clicks,2);
}
/* The shared graphics OBJECT is compiled without title-target macros. Its
 * weak callback resolves only in this D2R app, never in the generic shell. */
int dm_android_d2r_fps_setting(void) {int dm_fps_setting(void);return dm_fps_setting();}
const char *dm_android_custom_startup_locale(void) {
    /* app_jni is linked only by the D2R target. No guest access is needed
     * here: this executes before loading modules or initializing the SDK. */
    int selected=dm_custom_lang_explicit();
    static const char *const locales[]={"zh-Hans","en","es","ru"};
    return selected<0||selected>=4?NULL:locales[selected];
}
static int overlay_complete(const char *root) {
    static const char *files[]={"ctrl_touch","touch_strings.tsv","data/.build.config","data/00-00000007.data"};
    for(unsigned i=0;i<sizeof files/sizeof *files;i++) {
        char path[1200];struct stat st;
        if(snprintf(path,sizeof path,"%s/%s",root,files[i])>=(int)sizeof path || stat(path,&st) || !S_ISREG(st.st_mode) || st.st_size<=0 || access(path,R_OK))return 0;
    }
    return 1;
}
int dm_android_custom_overlay(const char *port_root,char *out,size_t capacity) {
    if(!port_root||!out||!capacity)return 0;out[0]=0;char selected[1100];
    if(dm_custom_get("item_notes")) {
        if(snprintf(selected,sizeof selected,"%s/romfs-overlay-variants/item_notes",port_root)<(int)sizeof selected && overlay_complete(selected)) {
            char feature[1200],key[64];snprintf(feature,sizeof feature,"%s/features.txt",selected);FILE *f=fopen(feature,"r");
            int exact=f&&fgets(key,sizeof key,f);if(f)fclose(f);if(exact){key[strcspn(key,"\r\n")]=0;exact=!strcmp(key,"item_notes");}
            if(exact && strlcpy(out,selected,capacity)<capacity){LOG("ANDROID_CUSTOM_OVERLAY item_notes=1 root=%s",out);return 1;}
        }
        LOG("ANDROID_CUSTOM_OVERLAY item_notes=1 complete_variant=0 fallback=base");
    }
    if(snprintf(selected,sizeof selected,"%s/romfs-overlay",port_root)>=(int)sizeof selected || !overlay_complete(selected) || strlcpy(out,selected,capacity)>=capacity){out[0]=0;LOG("ANDROID_CUSTOM_OVERLAY base_unavailable=1");return 0;}
    LOG("ANDROID_CUSTOM_OVERLAY item_notes=%d root=%s",dm_custom_get("item_notes"),out);return 1;
}
int dm_android_custom_attach(module_t *main) {
    if(!main||main!=&g_mod[0])return 0;title=main;
    if(!title_ok()||!gate_obs_present(OBS_UIMSG)||!gate_obs_present(OBS_TICK)||!gate_obs_present(OBS_C2S)||!gate_svc_present(SVC_HOST_C2S_DELIVER)){title=NULL;return 0;}
    return 1;
}
void dm_android_custom_scope(int enter) {
    if(!enter){scoped=0;return;}
    if(!title_ok())return;
    pthread_mutex_lock(&owner_lock);
    if(!have_owner){owner=pthread_self();have_owner=1;}
    scoped=pthread_equal(owner,pthread_self());pthread_mutex_unlock(&owner_lock);
}
static int name_read(uint64_t p,char out[64]) {
    if(!p){out[0]=0;return 1;}
    for(unsigned i=0;i<64;) {
        size_t bytes=16;if(bytes>64-i)bytes=64-i;
        size_t page=4096-((p+i)&4095);if(bytes>page)bytes=page;
        if(p>UINT64_MAX-i || !read_vm(p+i,out+i,bytes))return 0;
        if(memchr(out+i,0,bytes))return 1;i+=(unsigned)bytes;
    }
    out[63]=0;return 1;
}
static int add_node(context *c,uint64_t w,unsigned parent,unsigned index,unsigned depth,unsigned *result) {
    if(!w||depth>14||c->count>=NODE_MAX)return 0;
    for(unsigned i=0;i<c->count;i++)if(c->nodes[i].w==w)return 0;
    unsigned char header[0x68];if(!read_vm(w,header,sizeof header))return 0;
    unsigned at=c->count++;node *n=&c->nodes[at];memset(n,0,sizeof *n);
    n->w=w;n->parent=parent;n->index=index;n->depth=depth;
    memcpy(&n->vt,header,8);memcpy(&n->name,header+8,8);memcpy(&n->children,header+0x58,8);memcpy(&n->count,header+0x60,8);
    if(n->count>256||(n->count&&!n->children)||!name_read(n->name,n->text))return 0;
    *result=at;return 1;
}
static int children(context *c,unsigned parent,int recurse) {
    node n=c->nodes[parent];uint64_t list[256];
    if(n.count&&!read_vm(n.children,list,n.count*8))return 0;
    for(unsigned i=0;i<n.count;i++)if(list[i]) {
        unsigned child;if(!add_node(c,list[i],parent,i,n.depth+1,&child))return 0;
        if(recurse&&!children(c,child,1))return 0;
    }
    return 1;
}
static int epoch(context *c) {
    uint64_t root=0;
    return scoped&&title_ok()&&c->generation==dm_ui_gen()&&read_vm((uintptr_t)title->base+ROOT_OFF,&root,8)&&root==c->root;
}
static int locate(context *c,uint64_t w) {
    for(unsigned i=0;i<c->count;i++)if(c->nodes[i].w==w)return (int)i;
    return -1;
}
static int live(context *c,unsigned at) {
    if(!epoch(c))return 0;
    for(unsigned depth=0;depth<=14;depth++) {
        node *n=&c->nodes[at];uint64_t vt=0,name=0;
        if(!read_vm(n->w,&vt,8)||!read_vm(n->w+8,&name,8)||vt!=n->vt||name!=n->name)return 0;
        if(!at)return n->w==c->root;
        node *parent=&c->nodes[n->parent];uint64_t pair[2],entry=0;
        if(!read_vm(parent->w+0x58,pair,16)||pair[0]!=parent->children||pair[1]!=parent->count||n->index>=pair[1]||
           pair[0]>UINT64_MAX-n->index*8||!read_vm(pair[0]+n->index*8,&entry,8)||entry!=n->w)return 0;
        at=n->parent;
    }
    return 0;
}
static uint64_t find(void *opaque,uint64_t w,const char *name,int direct) {
    context *c=opaque;int start=locate(c,w);if(start<0||!live(c,(unsigned)start))return 0;
    for(unsigned i=0;i<c->count;i++)if(!strcmp(c->nodes[i].text,name)) {
        if(direct){if(i&&c->nodes[i].parent==(unsigned)start&&live(c,i))return c->nodes[i].w;}
        else {
            unsigned p=i;for(unsigned d=0;d<=14;d++) {
                if(p==(unsigned)start){if(live(c,i))return c->nodes[i].w;break;}
                if(!p)break;p=c->nodes[p].parent;
            }
        }
    }
    return 0;
}
static int read_field(void *opaque,uint64_t w,size_t off,void *out,size_t bytes) {
    context *c=opaque;int i=locate(c,w);if(i<0||!live(c,(unsigned)i)||w>UINT64_MAX-off)return 0;
    if(off==0xa88) {if(bytes!=1||c->nodes[i].vt!=(uintptr_t)title->base+DM_VT_TOGGLE)return 0;}
    else if(!((off==0x50&&bytes==1)||(off==0x30&&bytes==8)||(off==0x70&&bytes==16)))return 0;
    return read_vm(w+off,out,bytes);
}
static int write_vm(context *c,unsigned at,size_t off,const void *data,size_t bytes) {
    uint64_t w=c->nodes[at].w;unsigned char before[16];
    if(bytes>sizeof before||w>UINT64_MAX-off||!live(c,at)||!read_vm(w+off,before,bytes))return 0;
    struct iovec local={.iov_base=(void *)data,.iov_len=bytes},remote={.iov_base=(void *)(uintptr_t)(w+off),.iov_len=bytes};
    return process_vm_writev(getpid(),&local,1,&remote,1,0)==(ssize_t)bytes;
}
static int set_toggle(void *opaque,uint64_t w,int on) {
    context *c=opaque;int i=locate(c,w);if(i<0||c->nodes[i].vt!=(uintptr_t)title->base+DM_VT_TOGGLE)return 0;
    static const int32_t frames[2][4]={{0,1,3,2},{4,5,6,-1}};uint8_t value=on!=0;
    return write_vm(c,i,0xa88,&value,1)&&write_vm(c,i,0x6e8,frames[value],16)&&write_vm(c,i,0x610,frames[value],4);
}
static int set_rect(void *opaque,uint64_t w,const int32_t rect[4]) {
    context *c=opaque;int i=locate(c,w);if(i<0||strncmp(c->nodes[i].text,"TouchCustom",11)||!live(c,i))return 0;
    uint64_t base=(uintptr_t)title->base,vt=c->nodes[i].vt,offset=0,fn=0;
    /* Frozen 3.3.94012 RELATIVE relocations at each vtable+0x10. */
    if(vt==base+DM_VT_TEXT)offset=0xb79f50;
    else if(vt==base+NINE_TILE_VT)offset=0xb0a9ac;
    if(!offset||!read_vm(vt+0x10,&fn,8)||fn!=base+offset||!write_vm(c,i,0x70,rect,16))return 0;
    ((void(*)(uint64_t))(uintptr_t)fn)(w);return 1;
}
static int set_dirty(void *opaque,uint64_t w,int dirty) {
    context *c=opaque;int i=locate(c,w);if(i<0||strcmp(c->nodes[i].text,"VideoOptionsPanel")||!live(c,i))return 0;
    uint64_t base=(uintptr_t)title->base,vt=c->nodes[i].vt,table[96];
    if(vt<base||vt-base>title->size-sizeof table||!read_vm(vt,table,sizeof table))return 0;
    for(unsigned k=0;k<96;k++)if(table[k]==base+0xae5f28){uint8_t value=dirty!=0;return write_vm(c,i,0x1a0,&value,1);}
    return 0;
}
static int code(uint64_t p) {uint64_t b=(uintptr_t)title->base;return p>=b&&p-b<0x1b786a0;}
static void wheel(void *opaque,uint64_t w,int direction) {
    context *c=opaque;int i=locate(c,w);if(i<0||strcmp(c->nodes[i].text,"ScrollView")||!live(c,i)||!live(c,0))return;
    uint64_t base=(uintptr_t)title->base,vt=c->nodes[i].vt,root_vt=c->nodes[0].vt,hit=0,message=0;
    if(vt<base||vt-base>title->size-0x50||root_vt<base||root_vt-base>title->size-0x30||
       !read_vm(vt+0x48,&hit,8)||!read_vm(root_vt+0x28,&message,8)||!code(hit)||!code(message))return;
    int32_t dimensions[2];if(!read_vm(base+0x211da38,dimensions,8)||dimensions[0]<=0||dimensions[0]>8192||dimensions[1]<=0||dimensions[1]>8192)return;
    void dm_ui_wheel_on(uint64_t,int);dm_ui_wheel_on(w,direction);
}
static int pad(void *opaque) {(void)opaque;int dm_padmode_current(void);return dm_padmode_current();}
static uint64_t keys(void *opaque,uint64_t mask) {(void)opaque;uint64_t dm_padmode_page_keys(uint64_t);return dm_padmode_page_keys(mask);}
static uint64_t frame(void *opaque) {(void)opaque;uint64_t dm_guest_frame(void);return dm_guest_frame();}
static void platform_action(void *opaque,uint64_t w) {
    context *c=opaque;int i=locate(c,w);
    if(i<0||c->nodes[i].vt!=(uintptr_t)title->base+DM_VT_BUTTON||!live(c,(unsigned)i))return;
    if(!strcmp(c->nodes[i].text,"TouchCustom_mod")){void dm_custom_cycle_mod(void);dm_custom_cycle_mod();}
    if(!strcmp(c->nodes[i].text,"TouchCustom_map_reveal"))dm_android_map_request_when_ready(NULL);
}
static int clicked(void *opaque,uint64_t w) {
    context *c=opaque;int i=locate(c,w);if(i<0||!live(c,(unsigned)i))return 0;
    unsigned bit=!strcmp(c->nodes[i].text,"TouchCustom_mod")?1:!strcmp(c->nodes[i].text,"TouchCustom_map_reveal")?2:0;
    return bit&&(atomic_fetch_and(&action_clicks,~bit)&bit)!=0;
}
/* 文本只写入当前树中类型匹配的控件；地址来自 94012 的原版字符串赋值和刷新函数。
 * Text writes target matching live widgets only; addresses are 94012's string assignment and refresh functions. */
static int set_text(void *opaque,uint64_t w,const char *text) {
    context *c=opaque;int i=locate(c,w);uint64_t base=(uintptr_t)title->base,pair[2];
    if(i<0||(strcmp(c->nodes[i].text,"TouchCustom_mod_Text")&&strcmp(c->nodes[i].text,"TouchCustom_map_reveal_Text"))||c->nodes[i].vt!=base+DM_VT_TEXT||!live(c,(unsigned)i)||!read_vm(w+0x88,pair,16))return 0;
    size_t n=strlen(text);char previous[256];if(n>=sizeof previous)return 0;
    if(pair[1]==n&&read_vm(pair[0],previous,n)&&!memcmp(previous,text,n))return 1;
    if(!live(c,(unsigned)i))return 0;
    ((void *(*)(uint64_t,const char *,uint64_t))(base+0x136e5c))(w+0x88,text,n);
    if(!live(c,(unsigned)i))return 0;
    ((void (*)(uint64_t))(base+0xadc434))(w);return 1;
}
int dm_android_custom_ui_context(uint64_t root,dm_custom_ui_ops *out) {
    if(!scoped||!title_ok()||!root||!out)return 0;
    context *c=&current;memset(c,0,sizeof *c);c->root=root;c->generation=dm_ui_gen();
    unsigned root_index;if(!epoch(c)||!add_node(c,root,0,0,0,&root_index)||!children(c,0,0))return 0;
    unsigned count=c->count;int panel_visible=0;
    for(unsigned i=1;i<count;i++)if(!strcmp(c->nodes[i].text,"VideoOptionsPanel")) {
        uint8_t visible=0;if(!read_vm(c->nodes[i].w+0x50,&visible,1))return 0;
        panel_visible|=visible!=0;if(visible&&!children(c,i,1))return 0;
        if(!visible)atomic_store(&action_clicks,0);
    }
    if(!panel_visible)atomic_store(&action_clicks,0);
    *out=(dm_custom_ui_ops){.opaque=c,.generation=c->generation,.find=find,.read=read_field,.toggle=set_toggle,
        .rect=set_rect,.dirty=set_dirty,.wheel=wheel,.pad=pad,.keys=keys,.frame=frame,.action=platform_action,.clicked=clicked,.text=set_text};return epoch(c);
}
