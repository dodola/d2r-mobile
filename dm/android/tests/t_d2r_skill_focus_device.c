/* Real shipped dm_cursor helper and classifier, synthetic widget tree and
 * hit-test virtual method. No original game code or player data is opened. */
#include "../../host/dm.h"
#include "../../host/dm_cursor.h"
#include <dlfcn.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x))die("Skill focus fixture line=%d",__LINE__);}while(0)
static void put64(void *p,size_t off,uint64_t value){memcpy((uint8_t *)p+off,&value,8);}
static int hit(uint64_t w,uint64_t xy){
    (void)w;int32_t x=(int32_t)xy,y=(int32_t)(xy>>32);return x>=400 && x<=800 && y>=200 && y<=400;
}
static void widget(uint8_t *p,const char *name,uint64_t *vt){
    memset(p,0,0x600);put64(p,0,(uintptr_t)vt);put64(p,8,(uintptr_t)name);p[0x50]=p[0x51]=1;
}
int main(int argc,char **argv){
    if(argc!=2)return 2;void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!lib)die("dlopen %s",dlerror());
    module_t *mods=dlsym(lib,"g_mod");int *nmods=dlsym(lib,"g_nmod");
    int (*at)(float,float)=dlsym(lib,"dm_ui_item_at_now");
    uint32_t (*id)(void)=dlsym(lib,"dm_ui_hit_id");
    int (*skill)(uint32_t)=dlsym(lib,"dm_ui_hit_selected_skill");
    int (*kind)(float *,float *)=dlsym(lib,"dm_ui_bind_skill_hit");
    void (*frame)(void)=dlsym(lib,"dm_guest_frame_tick");
    uint32_t (*gen)(void)=dlsym(lib,"dm_ui_gen");
    void (*bump)(int)=dlsym(lib,"dm_ui_gen_bump");
    CHECK(mods && nmods && at && id && skill && kind && frame && gen && bump);
    *nmods=1;mods[0].size=0x3220000;mods[0].base=calloc(1,mods[0].size);CHECK(mods[0].base);
    strcpy(mods[0].name,"main");strcpy(mods[0].build_id,"db0507f36b84200ac05f75d76ed5026f");
    uint8_t root[0x600],tree[0x600],button[0x600],focus[0x600];
    uint64_t generic_vt[16]={0};generic_vt[9]=(uintptr_t)hit;
    uint64_t *button_vt=(uint64_t *)(mods[0].base+0x208a5f8);button_vt[9]=(uintptr_t)hit;
    widget(root,"root",generic_vt);widget(tree,"SkillsTreePanel",generic_vt);
    widget(button,"SkillButtonFixture",button_vt);widget(focus,"FocusManager",generic_vt);
    uint64_t panels[1]={(uintptr_t)tree},buttons[1]={(uintptr_t)button};
    put64(root,0x58,(uintptr_t)panels);put64(root,0x60,1);put64(root,0xd0,(uintptr_t)focus);
    put64(tree,0x58,(uintptr_t)buttons);put64(tree,0x60,1);
    int skill_number=17;put64(button,0x558,0xc50d69aa61850d12ull);put64(button,0x568,(uintptr_t)&skill_number);
    put64(focus,0x190,(uintptr_t)button);put64(mods[0].base,0x2f05458,(uintptr_t)root);
    int32_t dimensions[2]={1280,720};memcpy(mods[0].base+0x211da38,dimensions,sizeof dimensions);
    for(int i=0;i<3;i++){frame();gen();}
    CHECK(at(.45f,.35f)==0);uint32_t opaque=id();float x,y;
    CHECK(opaque==((uint32_t)(uintptr_t)button|0x80000000u));
    CHECK(kind(&x,&y)==1 && x>.44f && x<.46f && y>.34f && y<.36f);
    CHECK(skill(opaque)==17 && skill_number!=(int)opaque);
    CHECK(skill(opaque^0x100u)==-1 && skill(1)==-1 && skill(0)==-1);
    put64(focus,0x190,(uintptr_t)tree);CHECK(skill(opaque)==-1);put64(focus,0x190,(uintptr_t)button);
    put64(button,0x568,0);CHECK(skill(opaque)==-1);put64(button,0x568,(uintptr_t)&skill_number);
    put64(button,0x558,1);CHECK(skill(opaque)==-1);put64(button,0x558,0xc50d69aa61850d12ull);
    button[0x50]=0;CHECK(skill(opaque)==-1);button[0x50]=1;
    skill_number=23;CHECK(skill(opaque)==23);
    /* An address reused across generation changes is not a new hit. */
    bump(DM_UIGEN_LANG);CHECK(skill(opaque)==-1);
    for(int i=0;i<3;i++){frame();gen();}CHECK(skill(opaque)==-1);
    CHECK(at(.45f,.35f)==0 && id()==opaque && skill(opaque)==23);
    /* Removing the tree must reject before touching a stale focused widget. */
    put64(root,0x60,0);frame();gen();CHECK(skill(opaque)==-1);
    put64(focus,0x190,1);for(int i=0;i<3;i++){frame();gen();}CHECK(skill(opaque)==-1);
    put64(root,0x60,1);put64(focus,0x190,(uintptr_t)button);frame();gen();
    CHECK(skill(opaque)==-1);CHECK(at(.45f,.35f)==0 && skill(opaque)==23);
    put64(mods[0].base,0x2f05458,0);frame();gen();CHECK(skill(opaque)==-1);
    printf("ANDROID_D2R_SKILL_FOCUS_OK checks=%u real_shared_library=1 synthetic_widget_hit_test=1 opaque_identity_focus_match=1 generation_aba_rejected=1 original_game_ui_executed=0\n",checks);
    return 0;
}
