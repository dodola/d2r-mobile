/* Exercise the real D2R app library's version and UI gate adapters using only
 * a synthetic zeroed module image. No guest code, game resources or saves. */
#include "../../host/dm.h"
#include "../touch_controls_android.h"
#include <assert.h>
#include <dlfcn.h>

static uint64_t (*dispatch)(uint64_t,uint64_t *);
static void (*touch_tick)(void);
static void (*get_state)(dm_android_touch_game_state *);
static uint64_t hash(const char *s) {
    uint64_t h=0xcbf29ce484222325ull;
    while (*s) h=(h^(uint8_t)*s++)*0x100000001b3ull;
    return h;
}
static dm_android_touch_game_state message(const char *category,const char *action,const char *argument) {
    char text[65]={0}; snprintf(text,sizeof text,"%s",argument);
    uint64_t fields[]={hash(category),hash(action),(uintptr_t)text},registers[31]={0};
    registers[1]=(uintptr_t)fields;
    dispatch(0x1000,registers); touch_tick();
    dm_android_touch_game_state state; get_state(&state); return state;
}
int main(int argc,char **argv) {
    assert(argc==2);
    void *library=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if (!library) { fprintf(stderr,"dlopen: %s\n",dlerror()); return 1; }
    module_t *modules=dlsym(library,"g_mod"); int *count=dlsym(library,"g_nmod");
    int (*attach)(void)=dlsym(library,"dm_android_d2r_host_attach");
    int (*binding_attach)(void)=dlsym(library,"dm_touch_game_attach");
    void (*register_gate)(uint64_t,const char *,int,uint64_t)=dlsym(library,"gate_register");
    dispatch=dlsym(library,"gate_dispatch"); touch_tick=dlsym(library,"dm_android_touch_game_tick");
    get_state=dlsym(library,"dm_android_touch_get_game_state");
    assert(modules && count && attach && binding_attach && register_gate && dispatch && touch_tick && get_state);
    *count=1; strcpy(modules[0].name,"main"); modules[0].size=0x2f10000;
    modules[0].base=calloc(1,modules[0].size); assert(modules[0].base);
    register_gate(0x1000,"obs",OBS_UIMSG,0); register_gate(0x2000,"obs",6,0);
    assert(!attach() && !binding_attach()); /* Legacy identity, despite gates/size. */
    strcpy(modules[0].build_id,"2c7f979610e9911979982dc07bfc5702");
    assert(!attach() && !binding_attach()); /* Existing Android resource build. */
    strcpy(modules[0].build_id,"db0507f36b84200ac05f75d76ed5026f");
    size_t size=modules[0].size; modules[0].size=0x1000;
    assert(!attach() && !binding_attach()); modules[0].size=size;
    assert(attach() && binding_attach());
    dm_android_touch_game_state state=message("PanelManager","UnloadPanel","LoadScreenPanel");
    assert(!state.navigation && !state.panel_bits);
    state=message("PanelManager","OpenPanel","YouDiedModal");
    assert(state.navigation && (state.panel_bits&4) && !state.target_mask);
    state=message("PanelManager","ClosePanel","YouDiedModal");
    assert(!state.navigation && !state.panel_bits);
    state=message("PanelManager","OpenPanel","NpcTradePanel");
    assert(state.navigation && (state.panel_bits&2) && state.target_mask);
    state=message("PanelManager","UnloadAll","");
    assert(!state.navigation && !state.panel_bits);
    state=message("FrontEndNavigation","ToFrontEnd","");
    assert(state.navigation && !state.panel_bits);
    free(modules[0].base);
    puts("ANDROID_D2R_HOST_ATTACH_OK version_gates=8 ui_transitions=6 synthetic_image=1");
    return 0;
}
