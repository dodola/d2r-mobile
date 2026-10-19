/* Final app dispatcher with a complete private profile and a synthetic image.
 * Exercises gates without starting the guest, opening resources or saves. */
#include "../../host/dm.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned deliveries,systems;
static uint64_t seen[4];
static void deliver(uint64_t a,uint64_t b,uint64_t c,uint64_t d) {seen[0]=a;seen[1]=b;seen[2]=c;seen[3]=d;deliveries++;}
static void sysmsg(uint64_t a,uint64_t b) {seen[0]=a;seen[1]=b;systems++;}
static void trampoline(uint8_t *base,size_t offset,void *function) {
    uint32_t ins[]={0x58000050,0xd61f0200};memcpy(base+offset,ins,8);memcpy(base+offset+8,&function,8);
    __builtin___clear_cache((char *)base+offset,(char *)base+offset+16);
}
static void fill(uint64_t regs[31]) {for(unsigned i=0;i<31;i++)regs[i]=0x9988770000000000ull+i;}
int main(int argc,char **argv) {
    assert(argc==2);void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!library){fprintf(stderr,"dlopen: %s\n",dlerror());return 1;}
    module_t *mods=dlsym(library,"g_mod");int *count=dlsym(library,"g_nmod");
    int (*attach)(void)=dlsym(library,"dm_android_d2r_host_attach");
    int (*host)(int,uint64_t *)=dlsym(library,"dm_android_title_host_call");
    void (*gate)(uint64_t,const char *,int,uint64_t)=dlsym(library,"gate_register");
    int (*clock_override)(uint64_t,int64_t *)=dlsym(library,"dm_android_title_clock_override");
    uint64_t (*now)(void)=dlsym(library,"now_ns");
    _Atomic int64_t *tz=dlsym(library,"g_dm_tz_time");
    _Atomic uint64_t *deadline=dlsym(library,"g_dm_tz_live_ns"),*allow=dlsym(library,"g_dm_move_allow_until_ns");
    assert(mods && count && attach && host && gate && clock_override && now && tz && deadline && allow);
    *count=1;strcpy(mods[0].name,"main");strcpy(mods[0].build_id,"db0507f36b84200ac05f75d76ed5026f");
    mods[0].size=0x3212000;mods[0].base=mmap(NULL,mods[0].size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(mods[0].base!=MAP_FAILED);uint8_t *base=mods[0].base;
    trampoline(base,0x632510,deliver);trampoline(base,0x6310b0,sysmsg);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);assert(!mprotect(base+(0x6310b0&~(page-1)),2*page,PROT_READ|PROT_EXEC));
    gate(0x1000,"obs",OBS_UIMSG,0);gate(0x2000,"obs",OBS_TICK,0);gate(0x3000,"obs",OBS_C2S,0);
    const int numbers[]={0x81,0x82,0x85,0x89,0x8c,0x8d,0x8e};
    for(unsigned i=0;i<sizeof numbers/sizeof *numbers;i++)gate(0x4000+4*i,"svc",numbers[i],0);
    assert(attach());uint64_t regs[31],before[31];
    const int simple[]={0x85,0x89,0x8c,0x8d,0x8e};
    for(unsigned c=0;c<sizeof simple/sizeof *simple;c++) {
        fill(regs);if(simple[c]==0x8d)regs[0]=1;if(simple[c]==0x8e)regs[19]=0;
        memcpy(before,regs,sizeof regs);assert(host(simple[c],regs));assert(regs[8]==0);
        before[8]=0;assert(!memcmp(before,regs,sizeof regs));
    }
    fill(regs);memcpy(before,regs,sizeof regs);assert(!host(0x90,regs) && !memcmp(before,regs,sizeof regs));
    uint8_t packet[9]={0,0,0,0,1};regs[0]=(uintptr_t)packet;regs[1]=5;regs[2]=111;regs[3]=222;
    memcpy(before,regs,sizeof regs);assert(host(0x81,regs) && !deliveries);
    packet[4]=3;assert(host(0x81,regs) && !deliveries);
    packet[4]=2;assert(host(0x81,regs) && deliveries==1);
    assert(!memcmp(regs,seen,sizeof seen));
    packet[4]=0x42;assert(host(0x81,regs) && deliveries==2);
    packet[4]=1;atomic_store(allow,now()+1000000000ull);assert(host(0x81,regs) && deliveries==3);
    atomic_store(allow,0);regs[1]=0;assert(host(0x81,regs) && deliveries==4);regs[1]=5;
    assert(!memcmp(before,regs,sizeof regs));assert(host(0x82,regs) && systems==1);
    assert(seen[0]==regs[0] && seen[1]==regs[1] && !memcmp(before,regs,sizeof regs));
    int64_t value=0;uint64_t caller=(uintptr_t)base+0x58f220;
    assert(!clock_override(caller,&value));atomic_store(tz,123456789);atomic_store(deadline,now()+1000000000ull);
    assert(clock_override(caller,&value) && value==123456789);
    assert(!clock_override((uintptr_t)base+0x58f1e0,&value));
    assert(!clock_override((uintptr_t)base+0x591440,&value));
    assert(!clock_override((uintptr_t)base-4,&value));
    assert(!clock_override((uintptr_t)base+0x1b786a0,&value));
    assert(!clock_override(caller,NULL));
    uint32_t bl=0x94000000u|((0x58f310u-0x400000u)/4);memcpy(base+0x400000,&bl,4);
    assert(clock_override((uintptr_t)base+0x400000,&value) && value==123456789);
    bl=0xd503201f;memcpy(base+0x400000,&bl,4);assert(!clock_override((uintptr_t)base+0x400000,&value));
    atomic_store(deadline,now());assert(!clock_override(caller,&value));
    puts("ANDROID_D2R_PRODUCT_HOST_OK private_gates=5 movement_cases=6 system=1 clock_cases=10 registers_preserved=1");
    return 0;
}
