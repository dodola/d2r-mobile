/* Exercise the real D2R app dispatcher against synthetic ARM callees/objects.
 * No game process, resource, UI or save file is opened by this executable. */
#include "../../host/dm.h"
#include "../input_backend.h"
#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <sys/mman.h>
#include <unistd.h>

static uint8_t *main_image;
static uint8_t manager[0x1000], skill[0x20], unit[0x200], monster[0x200];
static uint16_t monster_path[4];
static uint64_t expected_self, expected_unit;
static unsigned originals;
static float range = 2.0f;
static int (*host_call)(int,uint64_t *);
static int (*publish)(uint64_t,int32_t,int32_t,int32_t,int32_t);
static int (*sample)(dm_android_npad_state *,uint32_t,uint32_t);
static void (*touch_tick)(void);

static int ready(void *opaque) { (void)opaque; return 1; }
static void original(uint64_t self,uint64_t player,float *position,float *direction) {
    assert(self==expected_self && player==expected_unit);
    position[0]=100.5f;position[1]=200.5f;
    direction[0]=0.25f;direction[1]=-0.75f;++originals;
}
static uint64_t get_manager(void) {return (uintptr_t)manager;}
static uint64_t get_skill(uint64_t self,int32_t id) {assert(self==(uintptr_t)manager && id==0);return (uintptr_t)skill;}
static float get_range(uint64_t self,uint64_t player,int hand) {
    assert(self==(uintptr_t)manager && player==(uintptr_t)unit && hand==0);return range;
}
static int hostile(int mask,uint64_t player,uint64_t candidate) {
    assert(mask==0x165 && player==(uintptr_t)unit && candidate==(uintptr_t)monster);return 1;
}
static int selectable(uint64_t candidate,int a,int b,int c,int d) {
    assert(candidate==(uintptr_t)monster && a==0 && b==0 && c==-1 && d==-1);return 1;
}
static void put(void *base,size_t offset,const void *value,size_t bytes) {memcpy((uint8_t *)base+offset,value,bytes);}
static void put64(void *base,size_t offset,uint64_t value) {put(base,offset,&value,8);}
static void put32(void *base,size_t offset,uint32_t value) {put(base,offset,&value,4);}
static void trampoline(size_t offset,void *function) {
    uint32_t ins[]={0x58000050,0xd61f0200}; /* ldr x16,.+8; br x16 */
    put(main_image,offset,ins,sizeof ins);put(main_image,offset+8,&function,sizeof function);
    __builtin___clear_cache((char *)main_image+offset,(char *)main_image+offset+16);
}
static void protect(size_t offset) {
    size_t page=(size_t)sysconf(_SC_PAGESIZE);
    assert(!mprotect(main_image+(offset&~(page-1)),page,PROT_READ|PROT_EXEC));
}
static void stick(int32_t x,int32_t y) {
    dm_android_npad_state state;
    assert(!publish(0,0,0,x,y) && !sample(&state,0,1));
    assert(state.attributes==1 && state.rx==x && state.ry==y);
}
static void call(uint64_t caller,uint32_t type,uint32_t id,float out[2]) {
    uint64_t regs[31],saved[31],frame[2]={0,(uintptr_t)main_image+caller};
    for(int i=0;i<31;i++)regs[i]=0x8877000000000000ull+i;
    float pos[2]={-1,-1};
    expected_self=regs[0]=0x1122334455667788ull;
    expected_unit=regs[1]=(uintptr_t)unit;
    regs[2]=(uintptr_t)pos;regs[3]=(uintptr_t)out;regs[23]=(uintptr_t)manager;regs[25]=0;regs[29]=(uintptr_t)frame;
    put32(unit,0,type);put32(unit,8,id);memcpy(saved,regs,sizeof regs);
    unsigned before=originals;
    assert(host_call(0x86,regs) && originals==before+1);
    assert(!memcmp(saved,regs,sizeof regs) && pos[0]==100.5f && pos[1]==200.5f);
}
static void near(float value,float expected) {assert(fabsf(value-expected)<0.0001f);}
int main(int argc,char **argv) {
    assert(argc==2);void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    module_t *modules=dlsym(library,"g_mod");int *count=dlsym(library,"g_nmod");
    int (*attach)(void)=dlsym(library,"dm_android_d2r_host_attach");
    void (*gate)(uint64_t,const char *,int,uint64_t)=dlsym(library,"gate_register");
    int (*bind)(const dm_android_input_config *)=dlsym(library,"dm_android_input_bind");
    int (*init)(void)=dlsym(library,"dm_android_input_initialize_npad");
    host_call=dlsym(library,"dm_android_title_host_call");
    publish=dlsym(library,"dm_android_input_publish_actions");sample=dlsym(library,"dm_android_input_read_npad");
    touch_tick=dlsym(library,"dm_android_touch_game_tick");
    assert(modules && count && attach && gate && bind && init && host_call && publish && sample && touch_tick);
    uint64_t absent_regs[31]={0};assert(!host_call(0x86,absent_regs));
    *count=1;strcpy(modules[0].name,"main");modules[0].size=0x2f10000;
    main_image=modules[0].base=mmap(NULL,modules[0].size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(main_image!=MAP_FAILED);
    trampoline(0x2b4ef8,original);trampoline(0x2acecc,get_manager);
    trampoline(0x2b4bc8,get_skill);trampoline(0x2b5c70,get_range);
    trampoline(0x2f7b7c,hostile);trampoline(0x464a70,selectable);
    protect(0x2b4ef8);protect(0x2acecc);protect(0x2b5c70);protect(0x2f7b7c);protect(0x464a70);
    strcpy(modules[0].build_id,"db0507f36b84200ac05f75d76ed5026f");
    gate(0x1000,"obs",OBS_UIMSG,0); /* Installs the guest HID pump callback. */
    assert(attach() && !host_call(0x86,absent_regs));
    gate(0x2000,"svc",0x86,0);
    dm_android_input_config config={.source="isolated-aim-fixture",.panel_width=1280,.panel_height=720,
        .source_ready=ready,.logical_npad_id=0,.logical_npad_style=1};
    assert(!bind(&config) && !init());
    put32(main_image,0x2d60380,42);put32(manager,0xd4c,0);
    float distance=2.0f;put(skill,8,&distance,4);unit[0x1bd]=1;
    uint8_t *tables=calloc(1,0x1200),*record=calloc(1,0x2ec);assert(tables && record);
    put64(main_image,0x2d64a18+16,(uintptr_t)tables);put64(tables,0x11e8,(uintptr_t)record);put64(tables,0x11f0,1);
    float out[2];stick(32767,0);
    call(0x2b352c,0,42,out);near(out[0],0.25f);near(out[1],-0.75f);
    call(0x2b5b80,1,42,out);near(out[0],0.25f);near(out[1],-0.75f);
    call(0x2b5b80,0,43,out);near(out[0],0.25f);near(out[1],-0.75f);
    stick(0,0);call(0x2b5b80,0,42,out);near(out[0],0.25f);near(out[1],-0.75f);

    /* No target: only the current skill's ignore-target flag is temporary.
     * The actual Android HID bridge pump must restore it after release. */
    stick(32767,0);call(0x2b5b80,0,42,out);
    near(out[0],sqrtf(0.5f)*12);near(out[1],-sqrtf(0.5f)*12);assert(skill[0x18]==1);
    stick(0,0);touch_tick();assert(skill[0x18]==0);
    stick(0,32767);call(0x2b3298,0,42,out);
    near(out[0],-sqrtf(0.5f)*12);near(out[1],-sqrtf(0.5f)*12);assert(skill[0x18]==1);
    stick(0,0);touch_tick();assert(skill[0x18]==0);

    /* Movement skills preserve their authored landing distance. */
    uint16_t movement=52;put(record,0x4e,&movement,2);
    stick(32767,0);call(0x2b3298,0,42,out);near(out[0],sqrtf(0.5f));near(out[1],-sqrtf(0.5f));
    stick(0,0);touch_tick();assert(skill[0x18]==0);
    movement=0;put(record,0x4e,&movement,2);
    distance=12.0f;put(skill,8,&distance,4);range=12.0f;
    stick(32767,0);call(0x2b5b80,0,42,out);near(out[0],sqrtf(0.5f));near(out[1],-sqrtf(0.5f));
    stick(0,0);touch_tick();

    /* A live monster in the cone is selected; release clears the designated
     * target slot even if no subsequent skill invocation happens. */
    put64(main_image,0x2d603a0+0x400,(uintptr_t)monster);
    put32(monster,0,1);put32(monster,4,123);put32(monster,8,99);put32(monster,0xc,1);
    monster_path[1]=103;monster_path[3]=197;put64(monster,0x38,(uintptr_t)monster_path);
    stick(32767,0);call(0x2b5b80,0,42,out);
    uint64_t target=0;memcpy(&target,manager+0xd10,8);assert(target==(uintptr_t)monster && !skill[0x18]);
    stick(0,0);touch_tick();memcpy(&target,manager+0xd10,8);assert(!target && !skill[0x18]);
    printf("ANDROID_D2R_AIM_OK original_calls=%u scope_guards=3 zero_axis=1 press_and_hold=2 distance_cases=3 cone_restore=5 gate_guards=2 registers_preserved=1\n",originals);
    return 0;
}
