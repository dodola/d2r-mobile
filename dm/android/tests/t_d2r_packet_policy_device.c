/* Shared Android product policy semantics with original ARM callees replaced
 * by controlled trampolines. No production process, resources or save files. */
#define DM_ANDROID_D2R_UI 1
#include "../d2r_host_modules.c"
#include <assert.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

static uint8_t player[0x200],item[0x200],cube[0x100],path[0x40];
static int live=1,automatic=1,belt_fits=1,normal_fits,has_cube=1,cube_fits=1,scroll,stackable;
static uint64_t stick_time,clock_time=10000000000ull;
_Atomic uint64_t g_dm_move_allow_until_ns;
int dm_tg_vm_read(uint64_t address,void *out,size_t bytes) {
    struct iovec local={out,bytes},remote={(void *)(uintptr_t)address,bytes};
    return address && process_vm_readv(getpid(),&local,1,&remote,1,0)==(ssize_t)bytes;
}
int dm_android_d2r_server_context(uint64_t *game,uint64_t *pl) { *game=live?1:0;*pl=live?(uintptr_t)player:0;return live; }
uint64_t dm_diag_srv_item(uint32_t id) {return live && id==99?(uintptr_t)item:0;}
int dm_potpick_auto(uint32_t id) {return automatic && id==99;}
uint64_t dm_stick_last_ns(void) {return stick_time;}
uint64_t now_ns(void) {return clock_time;}
static int belt(uint64_t inv,uint64_t it,uint32_t *slot,int empty) {assert(inv==123 && it==(uintptr_t)item && empty==1);*slot=6;return belt_fits;}
static int is_type(uint64_t it,int type) {assert(it==(uintptr_t)item);return scroll && type==0x16;}
static int stack(uint64_t it) {assert(it==(uintptr_t)item);return stackable;}
static d2r_autoplace_result place(uint64_t game,uint64_t pl,uint64_t it) {
    assert(game==321 && pl==(uintptr_t)player && it==(uintptr_t)item);
    d2r_autoplace_result result={0};result.ok=normal_fits;return result;
}
static uint64_t get_cube(uint64_t pl) {assert(pl==(uintptr_t)player);return has_cube?(uintptr_t)cube:0;}
static int cube_space(uint64_t inv,uint64_t it,int search,uint32_t *x,uint32_t *y,int page) {
    assert(inv==123 && it==(uintptr_t)item && search==9 && page==3);*x=2;*y=3;return cube_fits;
}
static void put64(void *object,size_t offset,uint64_t value) {memcpy((uint8_t *)object+offset,&value,8);}
static void put32(void *object,size_t offset,uint32_t value) {memcpy((uint8_t *)object+offset,&value,4);}
static uint32_t get32(const void *object,size_t offset) {uint32_t value;memcpy(&value,(const uint8_t *)object+offset,4);return value;}
static void trampoline(uint8_t *base,size_t offset,void *function) {
    uint32_t ins[]={0x58000050,0xd61f0200};memcpy(base+offset,ins,8);memcpy(base+offset+8,&function,8);
    __builtin___clear_cache((char *)base+offset,(char *)base+offset+16);
    size_t page=(size_t)sysconf(_SC_PAGESIZE);assert(!mprotect(base+(offset&~(page-1)),page,PROT_READ|PROT_EXEC));
}
int main(void) {
    module_t main={0};main.size=0x650000;main.base=mmap(NULL,main.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(main.base!=MAP_FAILED);g_d2r_main=&main;
    trampoline(main.base,0x561de0,belt);trampoline(main.base,0x574bc0,is_type);trampoline(main.base,0x5722f0,stack);
    trampoline(main.base,0x646840,place);trampoline(main.base,0x565e50,get_cube);trampoline(main.base,0x55f620,cube_space);
    put64(player,0x90,123);put64(player,0xd8,321);put32(item,0xc,3);
    put32(cube,8,77);put64(cube,0x38,(uintptr_t)path);put32(path,0x10,7);put32(path,0x14,8);
    uint8_t packet[21]={0xaa,0xbb,0xcc,0xdd,0x16},out[29];put32(packet,5,99);put32(packet,9,55);put32(packet,13,66);put32(packet,17,1);
    uint64_t regs[31]={0},before[31];regs[0]=(uintptr_t)packet;regs[1]=17;regs[2]=1234;regs[3]=5678;memcpy(before,regs,sizeof regs);
    assert(!d2r_potpick_gate(regs));belt_fits=0;assert(d2r_potpick_gate(regs));
    automatic=0;assert(!d2r_potpick_gate(regs));automatic=1;live=0;assert(!d2r_potpick_gate(regs));live=1;
    put32(item,0xc,0);assert(!d2r_potpick_gate(regs));put32(item,0xc,3);
    assert(d2r_cube_pickup(regs,out));
    assert(!memcmp(packet,out,4) && out[4]==0x5f && get32(out,5)==99 && get32(out,9)==77);
    assert(get32(out,13)==55 && get32(out,17)==66 && get32(out,21)==(7u|8u<<16) && get32(out,25)==(2u|3u<<16));
    normal_fits=1;assert(!d2r_cube_pickup(regs,out));normal_fits=0;
    has_cube=0;assert(!d2r_cube_pickup(regs,out));has_cube=1;
    cube_fits=0;assert(!d2r_cube_pickup(regs,out));cube_fits=1;
    scroll=1;assert(!d2r_cube_pickup(regs,out));scroll=0;
    stackable=1;assert(!d2r_cube_pickup(regs,out));stackable=0;
    put32(packet,17,0);assert(!d2r_cube_pickup(regs,out));put32(packet,17,1);
    put32(item,0xc,0);assert(!d2r_cube_pickup(regs,out));put32(item,0xc,3);
    live=0;assert(!d2r_cube_pickup(regs,out));live=1;
    put32(packet,5,100);assert(!d2r_cube_pickup(regs,out));put32(packet,5,99);
    regs[1]=16;assert(!d2r_cube_pickup(regs,out) && !d2r_potpick_gate(regs));regs[1]=17;
    packet[4]=1;assert(d2r_drop_ground_move(regs));packet[4]=3;assert(d2r_drop_ground_move(regs));
    packet[4]=2;assert(!d2r_drop_ground_move(regs));packet[4]=0x42;assert(!d2r_drop_ground_move(regs));
    packet[4]=1;stick_time=clock_time-700000000ull;assert(!d2r_drop_ground_move(regs));
    stick_time--;assert(d2r_drop_ground_move(regs));
    atomic_store(&g_dm_move_allow_until_ns,clock_time+1);assert(!d2r_drop_ground_move(regs));
    atomic_store(&g_dm_move_allow_until_ns,clock_time);assert(d2r_drop_ground_move(regs));
    assert(!memcmp(before,regs,sizeof regs));
    puts("ANDROID_D2R_PACKET_POLICY_OK potion_cases=6 cube_cases=11 movement_cases=8 rewrite_bytes=29 registers_preserved=1");
    return 0;
}
