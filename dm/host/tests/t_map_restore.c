/* 直接运行地图泵，验证配置不改写、面板/换区恢复与过场按键；Linux 合成客体，无游戏内容。
 * Runs the actual map pump to verify preserved configuration, panel/area recovery and cutscene keys; synthetic Linux guest, no game content. */
#include <assert.h>
#include <sys/mman.h>
#include "../dm_cursor.c"
module_t g_mod[MAX_MOD];int g_nmod;
static dm_hud_info hud={.valid=1};static int pad,ingame=1;static uint64_t frame=100;
void dm_hud_get(dm_hud_info *out){*out=hud;}
int dm_padmode_current(void){return pad;}
int dm_uiscreen_ingame(void){return ingame;}
uint64_t dm_guest_frame(void){return frame;}
static void map_callback(int mode){*(int32_t*)(g_mod[0].base+0x2d5c748)=mode!=0;}
int main(void){
    size_t size=0x2f05460;uint8_t *base=mmap(NULL,size,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(base!=MAP_FAILED);
    g_nmod=1;strcpy(g_mod[0].name,"main");g_mod[0].base=base;
    uint8_t root[160]={0};
    uint8_t jump[12]={0x48,0xb8};uintptr_t fn=(uintptr_t)map_callback;memcpy(jump+2,&fn,8);jump[10]=0xff;jump[11]=0xe0;memcpy(base+0x40f780,jump,12);
    int32_t *cfg=(int32_t*)(base+0x2d479f4),*mini=(int32_t*)(base+0x2d5c748);*cfg=0;*mini=1;
    ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!*mini&&!*cfg&&dm_ui_map_is_full());
    dm_ui_map_hold(1);ui_map_pump(&g_mod[0],(uintptr_t)root);assert(*mini&&!*cfg&&!dm_ui_map_is_full());
    dm_ui_map_hold(0);ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!*mini&&!*cfg);
    *mini=1;ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!*mini);
    dm_ui_map_full(0);ui_map_pump(&g_mod[0],(uintptr_t)root);assert(*mini&&!*cfg);
    *cfg=2;ui_map_pump(&g_mod[0],(uintptr_t)root);assert(*mini&&*cfg==2);
    dm_ui_map_full(1);ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!*mini&&*cfg==2);
    uint8_t am[160]={0};uint64_t child=(uintptr_t)am;
    *(uint64_t*)(root+0x58)=(uintptr_t)&child;*(uint64_t*)(root+0x60)=1;
    *(uint64_t*)am=(uintptr_t)base+0x20c58a0;*(const char**)(am+8)="AutoMap";
    *(int32_t*)(base+0x2d485b4)=1;atomic_store(&g_map_movie,1);
    ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!atomic_load(&g_alone_mask));
    atomic_store(&g_map_movie,0);ui_map_pump(&g_mod[0],(uintptr_t)root);assert(atomic_load(&g_alone_mask)==DM_BTN_MINUS);
    atomic_store(&g_alone_mask,0);frame+=200;pad=1;ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!atomic_load(&g_alone_mask));
    pad=0;*(int32_t*)(base+0x2d485b4)=0;ui_map_pump(&g_mod[0],(uintptr_t)root);assert(!atomic_load(&g_alone_mask));
    hud.valid=ingame=0;ui_map_pump(&g_mod[0],(uintptr_t)root);assert(g_map_cfg_seen==-1&&!dm_ui_map_is_full());
    munmap(base,size);puts("MAP_RESTORE PASS config panels area cutscene controller option run_reset");
}
