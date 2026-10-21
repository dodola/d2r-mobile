#include "d2r_save_adapter.h"
#include "lifecycle_android.h"
#include "../host/dm_hud.h"
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef DM_ANDROID_HUD_EXP_HEADER
#include DM_ANDROID_HUD_EXP_HEADER
_Static_assert(sizeof k_exp / sizeof k_exp[0] == 100, "D2R generated experience table");
#define HUD_EXPERIENCE 1
#else
#define HUD_EXPERIENCE 0
#endif
/* Packaging can check this read-only ELF datum without loading Android code.
 * It follows the same compile-time branch as the actual HUD XP consumer. */
const uint32_t dm_android_hud_exp_table_entries = HUD_EXPERIENCE ? 100 : 0;

/* These offsets are shared with diag.c's reviewed D2R tick/HUD producer.
 * Hosts without a reviewed D2R adapter never link this file. Reads and writes use the kernel's checked
 * same-process copy API so a stale guest heap pointer does not fault here. */
#define D2R_BUILD_ID "db0507f36b84200ac05f75d76ed5026f"
enum {
    LOCAL_SLOT = 0x2d20e4c, LOCAL_IDS = 0x2d60380, CLIENT_TABLES = 0x2d603a0,
    RUN_SLOTS = 0x2d665d8, RUN_COUNT = 1024,
    PAUSE_MENU = 0x2d5ff51, PAUSE_RIFT = 0x2d5ff67,
    QUEST_HINT = 0x2d5ff58,
    SAVE_TIME = 0x5800, PLAYER_BUCKETS = 0x2230,
    WORLD_STEP = 0x20cebb8, WORLD_TIME = 0x3114208
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static module_t *image;
static uint64_t last_game, last_runs;
static uint64_t requested_game, requested_runs;
static int last_players;
static int freeze_gates, background_active, restore_pending;
static _Atomic int frozen;
static uint64_t background_game, background_runs, frozen_world_time;
static uint8_t background_pause[2];
/* Restoring pause can race a scheduler which already passed its pause check.
 * Keep that run frozen while the restored pause remains set. Its first real
 * resume clears this guard, including a tick that entered after rollback. */
static uint64_t paused_game, paused_runs, paused_world_time;
_Atomic int g_dm_frz_want;
static int panel_active, panel_idle;
static uint64_t panel_game, panel_runs, panel_world_time;
/* A wait may finish between tick entry and world-sim entry. That tick must
 * still use its stand-in after the UI restores pause and releases frozen.
 * The first genuine tick after resume replaces this per-thread decision. */
static __thread uint64_t frozen_tick_game, frozen_tick_runs;
/* No large temporaries on the original SDK's guest stack. */
static uint64_t run_slots[RUN_COUNT];
static uint32_t stat_pairs[512][2];
static int in_game_locked(void);
void dm_android_d2r_rift_tick(uint64_t game, uint64_t player, uint8_t *main_base)
    __attribute__((weak));
int dm_android_d2r_players_get(uint8_t *main_base) __attribute__((weak));
void dm_android_d2r_ui_tick(uint64_t game,uint64_t player,uint8_t *main_base)
    __attribute__((weak));
void dm_android_d2r_debug_tick(uint64_t game,uint64_t player,uint8_t *main_base,uint64_t runs)
    __attribute__((weak));

static int read_memory(uint64_t address, void *out, size_t bytes) {
    if (!address || !out || !bytes || address > UINT64_MAX - bytes) return 0;
    struct iovec local = {.iov_base=out, .iov_len=bytes};
    struct iovec remote = {.iov_base=(void *)(uintptr_t)address, .iov_len=bytes};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)bytes;
}
static int read_field(uint64_t object, size_t offset, void *out, size_t bytes) {
    return object && object <= UINT64_MAX - offset &&
        read_memory(object + offset, out, bytes);
}
static int write_memory(uint64_t address, const void *value, size_t bytes) {
    if (!address || !value || !bytes || address > UINT64_MAX - bytes) return 0;
    dm_guest_write_prepare((void *)(uintptr_t)address, bytes);
    struct iovec local = {.iov_base=(void *)value, .iov_len=bytes};
    struct iovec remote = {.iov_base=(void *)(uintptr_t)address, .iov_len=bytes};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)bytes;
}
/* Record the live run-number cohort along with G. A new run can occupy the
 * slots before its first observer call; that must not revive the old G. */
static uint64_t live_runs(void) {
    if (!image || !read_field((uint64_t)image->base, RUN_SLOTS,
                              run_slots, sizeof run_slots)) return 0;
    uint64_t hash = 0xcbf29ce484222325ull;
    unsigned count = 0;
    for (unsigned i=0; i<RUN_COUNT; ++i) if (run_slots[i] && run_slots[i] != UINT64_MAX) {
        hash = (hash ^ i) * 0x100000001b3ull;
        hash = (hash ^ run_slots[i]) * 0x100000001b3ull;
        ++count;
    }
    return count ? (hash ? hash : 1) : 0;
}
extern uint64_t dm_nvn_frame_count(void) __attribute__((weak));
/* The freeze hooks run for every hooked game call, many times per frame, and each live_runs() is a process_vm_readv of
 * 8 KB plus a 1024-slot hash. The run table only changes when a run starts or ends, so one read per presented frame is
 * enough (a change is noticed at most one frame later). DM_DISABLE_RUNS_CACHE restores a read per call. */
static uint64_t live_runs_frame_cached(void) {
    static uint64_t cached_frame=UINT64_MAX,cached_value,cached_ms;static int off=-1;
    if(off<0)off=getenv("DM_DISABLE_RUNS_CACHE")!=NULL;
    uint64_t frame=(!off&&dm_nvn_frame_count)?dm_nvn_frame_count():UINT64_MAX;
    /* 帧计数只在出图时前进（加载画面、后台、丢失 Surface 时不动），所以再加一个 50 ms 的时间上限。
     * The frame count only advances on presents (not during loading screens, in the background or without a Surface), so the
     * cache is also bounded to 50 ms. */
    struct timespec ts;clock_gettime(CLOCK_MONOTONIC_COARSE,&ts);
    uint64_t now_ms=(uint64_t)ts.tv_sec*1000u+(uint64_t)ts.tv_nsec/1000000u;
    if(frame!=UINT64_MAX&&frame==cached_frame&&now_ms-cached_ms<50)return cached_value;
    uint64_t value=live_runs();cached_frame=frame;cached_value=value;cached_ms=now_ms;return value;
}
static void clear_freeze(void) {
    background_active=restore_pending=0;
    background_game=background_runs=0;
    atomic_store(&frozen,0);
}
static void clear_run(void) {
    /* A new run owns the UI bytes now; never restore the old run's pause. */
    clear_freeze();
    paused_game=paused_runs=0;
    panel_active=panel_idle=0;
    frozen_tick_game=frozen_tick_runs=0;
    last_game = last_runs = 0;
    last_players=0;
    requested_game = requested_runs = 0;
    dm_hud_info empty = {0}; dm_hud_publish(&empty); dm_hud_set_held(0);
}
static void restore_pause_locked(void) {
    requested_game=requested_runs=0;
    if (!background_active) return;
    if (last_game != background_game || live_runs() != background_runs) {
        clear_freeze(); return;
    }
    restore_pending=1;
    if (!write_memory((uint64_t)image->base+PAUSE_MENU,&background_pause[0],1) ||
        !write_memory((uint64_t)image->base+PAUSE_RIFT,&background_pause[1],1)) {
        LOG("ANDROID_D2R_PAUSED_SAVE restore_pending=1 world_still_frozen=1");
        return;
    }
    /* The original pause is visible before the world can run again. */
    paused_game=background_game; paused_runs=background_runs;
    paused_world_time=frozen_world_time;
    clear_freeze();
    LOG("ANDROID_D2R_PAUSED_SAVE restored_pause=1 thawed=1");
}
static int frozen_now_locked(void) {
    uint64_t runs=live_runs_frame_cached();
    if (background_active && (last_game!=background_game || runs!=background_runs)) clear_freeze();
    if (atomic_load(&frozen)) return 1;
    if (paused_game) {
        uint8_t pause=0, rift=0;
        if (runs==paused_runs &&
            read_field((uint64_t)image->base,PAUSE_MENU,&pause,1) &&
            read_field((uint64_t)image->base,PAUSE_RIFT,&rift,1) && (pause || rift)) return 1;
        paused_game=paused_runs=0;
    }
    if (panel_active && (last_game!=panel_game || runs!=panel_runs)) panel_active=0;
    return panel_active;
}
static void freeze_world(const uint64_t *registers) {
    static uint64_t queue[0x1500/8], game[0x200/8];
    pthread_mutex_lock(&lock);
    if (restore_pending) restore_pause_locked();
    if (frozen_now_locked() || (registers[0]==frozen_tick_game && frozen_tick_game &&
        live_runs_frame_cached()==frozen_tick_runs)) {
        game[0x170/8]=0;
        game[0x188/8]=(uintptr_t)queue;
        ((uint64_t *)registers)[0]=(uintptr_t)game;
    }
    pthread_mutex_unlock(&lock);
}
static void freeze_client(const uint64_t *registers) {
    static uint32_t unit[0x200/4];
    pthread_mutex_lock(&lock);
    if (frozen_now_locked() && registers[0]) {
        unit[0x124/4]=1u<<23;
        ((uint64_t *)registers)[0]=(uintptr_t)unit;
    }
    pthread_mutex_unlock(&lock);
}
static void freeze_animation(const uint64_t *registers) {
    static uint32_t unit[0x200/4];
    pthread_mutex_lock(&lock);
    if (frozen_now_locked() && registers[0]) {
        unit[0]=9;
        ((uint64_t *)registers)[0]=(uintptr_t)unit;
    }
    pthread_mutex_unlock(&lock);
}
static void freeze_render(const uint64_t *registers) {
    (void)registers;
    pthread_mutex_lock(&lock);
    if (restore_pending) restore_pause_locked();
    if (frozen_now_locked()) {
        uint32_t zero=0;
        uint64_t time=atomic_load(&frozen)?frozen_world_time:
            paused_game?paused_world_time:panel_world_time;
        write_memory((uint64_t)image->base+WORLD_STEP,&zero,4);
        write_memory((uint64_t)image->base+WORLD_TIME,&time,8);
    }
    pthread_mutex_unlock(&lock);
}
static int freeze_ready(void) {
    if (freeze_gates) return 1;
    if (image->size < WORLD_TIME+8 || !gate_obs_present(OBS_FREEZE_WORLD) ||
        !gate_obs_present(OBS_FREEZE_CLIENT) || !gate_obs_present(OBS_FREEZE_ANIM) ||
        !gate_obs_present(OBS_FREEZE_RENDER)) return 0;
    gate_obs_set(OBS_FREEZE_WORLD,freeze_world);
    gate_obs_set(OBS_FREEZE_CLIENT,freeze_client);
    gate_obs_set(OBS_FREEZE_ANIM,freeze_animation);
    gate_obs_set(OBS_FREEZE_RENDER,freeze_render);
    freeze_gates=1; return 1;
}
static void panel_tick_locked(uint64_t game,uint64_t runs,uint32_t tick) {
    const char *option=getenv("DM_PANEL_FREEZE");
    int want=option && *option && *option!='0' && atomic_load(&g_dm_frz_want);
    if (!want || (panel_active && (game!=panel_game || runs!=panel_runs))) panel_active=0;
    panel_idle=0;
    if (!want || panel_active || !freeze_ready()) return;
    /* Match diag.c: periodic tasks at these tick numbers must finish before
     * a panel stops the server counter. Background save is already frozen. */
    if (atomic_load(&frozen) || tick%20==0 || tick%12==0 || tick%11==0 || tick%1500==0) return;
    if (read_field((uint64_t)image->base,WORLD_TIME,&panel_world_time,8)) {
        panel_game=game; panel_runs=runs; panel_active=1;
    }
}
void dm_freeze_frame(int want) {
    pthread_mutex_lock(&lock);
    if (!panel_active || want) panel_idle=0;
    else if (++panel_idle>=10) { panel_active=panel_idle=0; }
    pthread_mutex_unlock(&lock);
}
static int begin_wait(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&lock);
    if (restore_pending) restore_pause_locked();
    int answer=0;
    uint8_t pause[2]; uint32_t tick=0, step=0;
    uint64_t timer=0, world_time=0;
    uint64_t base=image?(uintptr_t)image->base:0;
    if (background_active || !in_game_locked() ||
        !read_field(base,PAUSE_MENU,&pause[0],1) || !read_field(base,PAUSE_RIFT,&pause[1],1)) {
        answer=-1; goto done;
    }
    if (!pause[0] && !pause[1]) goto done;
    answer=-1;
    if (!freeze_ready() || !read_field(last_game,SAVE_TIME,&timer,8) || !timer ||
        !read_field(last_game,0x170,&tick,4) || !read_field(base,WORLD_STEP,&step,4) ||
        !read_field(base,WORLD_TIME,&world_time,8) ||
        !write_memory(base+PAUSE_MENU,&pause[0],1) || !write_memory(base+PAUSE_RIFT,&pause[1],1) ||
        !write_memory(base+WORLD_STEP,&step,4) || !write_memory(base+WORLD_TIME,&world_time,8) ||
        !write_memory(last_game+0x170,&tick,4)) goto done;
    background_game=last_game; background_runs=last_runs;
    memcpy(background_pause,pause,sizeof pause); frozen_world_time=world_time;
    background_active=1;
    atomic_store(&frozen,1);
    uint8_t zero=0;
    if (!write_memory(base+PAUSE_MENU,&zero,1) || !write_memory(base+PAUSE_RIFT,&zero,1)) {
        restore_pause_locked(); goto done;
    }
    answer=1;
    LOG("ANDROID_D2R_PAUSED_SAVE all_four_gates=1 frozen_before_unpause=1");
done:
    pthread_mutex_unlock(&lock); return answer;
}
static void end_wait(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&lock); restore_pause_locked(); pthread_mutex_unlock(&lock);
}
static int in_game_locked(void) {
    uint64_t runs = last_game ? live_runs() : 0;
    uint32_t tick;
    if (!runs || runs != last_runs ||
        !read_field(last_game, 0x170, &tick, sizeof tick)) {
        clear_run(); return 0;
    }
    return 1;
}
static int game_callback(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&lock);
    int answer = in_game_locked();
    pthread_mutex_unlock(&lock); return answer;
}
static int save_callback(void *opaque) {
    (void)opaque;
    pthread_mutex_lock(&lock);
    uint8_t pause=0, rift=0;
    uint64_t last=0;
    int ok = in_game_locked() &&
        read_field((uint64_t)image->base, PAUSE_MENU, &pause, 1) &&
        read_field((uint64_t)image->base, PAUSE_RIFT, &rift, 1) &&
        !pause && !rift &&
        read_field(last_game, SAVE_TIME, &last, sizeof last) && last;
    if (ok) { requested_game=last_game; requested_runs=last_runs; }
    /* Without the complete four-gate world freeze we never release either
     * pause flag. A paused run is reported as an unfulfilled save request. */
    pthread_mutex_unlock(&lock); return ok;
}
static void apply_save_request(uint64_t game, uint64_t runs) {
    if (!requested_game) return;
    uint8_t pause=0, rift=0; uint64_t last=0, value=1;
    /* Only the observed original tick thread writes G. The UI request can
     * race a run ending; an old request must never write a recycled object. */
    int ok=game == requested_game && runs == requested_runs &&
        read_field((uint64_t)image->base, PAUSE_MENU, &pause, 1) &&
        read_field((uint64_t)image->base, PAUSE_RIFT, &rift, 1) && !pause && !rift &&
        read_field(game, SAVE_TIME, &last, sizeof last) && last &&
        game <= UINT64_MAX - SAVE_TIME && write_memory(game + SAVE_TIME, &value, sizeof value);
    requested_game=requested_runs=0;
    LOG("ANDROID_D2R_SAVE_APPLY requested=1 applied=%d",ok);
}
static int stat_value(uint64_t unit, unsigned stat, int totals, uint32_t *out) {
    uint64_t list=0;
    int32_t flags=0;
    struct { uint64_t address, count; } span;
    if (!read_field(unit, 0x88, &list, 8) || !list ||
        (totals && !read_field(list, 0x1c, &flags, 4)) ||
        !read_field(list, totals && flags < 0 ? 0xe8 : 0x30, &span, sizeof span) ||
        span.count > 512 || !span.count ||
        !read_memory(span.address, stat_pairs, (size_t)span.count * sizeof stat_pairs[0])) return 0;
    for (uint64_t i=0; i<span.count; ++i)
        if (stat_pairs[i][0] == stat << 16) { *out=stat_pairs[i][1]; return 1; }
    return 0;
}
uint32_t dm_cur_tick(void) {
    pthread_mutex_lock(&lock);
    uint32_t tick=0;
    if (in_game_locked()) read_field(last_game,0x170,&tick,4);
    pthread_mutex_unlock(&lock); return tick;
}
int dm_diag_difficulty(void) {
    pthread_mutex_lock(&lock);
    uint8_t difficulty=3;
    int value=in_game_locked() && read_field(last_game,0x104,&difficulty,1) && difficulty<3?
        difficulty:-1;
    pthread_mutex_unlock(&lock); return value;
}
static uint64_t server_unit(unsigned type,uint32_t id) {
    uint64_t unit=0;
    size_t buckets=type==4?0x2e30:0x2630;
    if (!read_field(last_game,buckets+8*(id&127),&unit,8)) return 0;
    for (unsigned n=0;unit && n<4096;++n) {
        uint32_t fields[3]; uint64_t next=0;
        if (!read_memory(unit,fields,sizeof fields)) return 0;
        if (fields[0]==type && fields[2]==id) return unit;
        if (!read_field(unit,0x158,&next,8) || next==unit) return 0;
        unit=next;
    }
    return 0;
}
uint64_t dm_diag_srv_item(uint32_t id) {
    pthread_mutex_lock(&lock);
    uint64_t unit=in_game_locked()?server_unit(4,id):0;
    pthread_mutex_unlock(&lock); return unit;
}
int dm_diag_srv_merc(uint32_t id,int *hp,int *maximum,int *healing) {
    if (!hp || !maximum || !healing) return 0;
    pthread_mutex_lock(&lock);
    int ok=0;
    uint64_t unit=in_game_locked()?server_unit(1,id):0, list=0;
    uint32_t mode=0, life=0, max_life=0, flags=0, states=0;
    if (unit && read_field(unit,0xc,&mode,4) && mode && mode!=12 &&
        stat_value(unit,6,1,&life) && stat_value(unit,7,1,&max_life) &&
        life<=INT_MAX && max_life && max_life<=INT_MAX) {
        *hp=(int)life/256; *maximum=(int)max_life/256;
        *healing=read_field(unit,0x88,&list,8) && list &&
            read_field(list,0x1c,&flags,4) && (flags>>31) &&
            read_field(list,0xb30+4*(100>>5),&states,4) && (states>>(100&31)&1);
        ok=1;
    }
    pthread_mutex_unlock(&lock); return ok;
}
static uint64_t controlled_player(uint64_t game) {
    uint32_t slot=8, id=UINT32_MAX;
    if (!read_field((uint64_t)image->base, LOCAL_SLOT, &slot, 4) || slot >= 8 ||
        !read_field((uint64_t)image->base, LOCAL_IDS + 4 * slot, &id, 4) ||
        id == UINT32_MAX) return 0;
    uint64_t unit=0;
    if (!read_field(game, PLAYER_BUCKETS + 8 * (id & 127), &unit, 8)) return 0;
    for (unsigned visited=0; unit && visited<4096; ++visited) {
        uint32_t fields[4];
        if (!read_memory(unit, fields, sizeof fields)) return 0;
        if (!fields[0] && fields[2] == id) return unit;
        uint64_t next=0;
        if (!read_field(unit, 0x158, &next, 8) || next == unit) return 0;
        unit=next;
    }
    return 0;
}
int dm_android_d2r_server_context(uint64_t *game,uint64_t *player) {
    if (!game || !player) return 0;
    *game=*player=0;
    pthread_mutex_lock(&lock);
    if (in_game_locked()) {
        *player=controlled_player(last_game);
        if (*player) *game=last_game;
    }
    int ok=*game && *player;
    pthread_mutex_unlock(&lock); return ok;
}
/* Same client cursor validation as dm_actions_pump, without guest calls.
 * A server inventory may differ from the client cursor while dragging. */
static uint64_t client_unit(unsigned type, uint32_t id) {
    uint64_t unit=0;
    if (type > 4 || !read_field((uint64_t)image->base,
        CLIENT_TABLES + type * 0x400 + 8 * (id & 127), &unit, 8)) return 0;
    for (unsigned visited=0; unit && visited<4096; ++visited) {
        uint32_t fields[3]; uint64_t next=0;
        if (!read_memory(unit, fields, sizeof fields)) return 0;
        if (fields[0] == type && fields[2] == id) return unit;
        if (!read_field(unit, 0x158, &next, 8) || next == unit) return 0;
        unit=next;
    }
    return 0;
}
static void publish_held(uint64_t player) {
    uint32_t id=0, signature=0, fields[3];
    uint64_t client=0, inventory=0, held=0;
    if (read_field(player, 8, &id, 4) && (client=client_unit(0,id)) &&
        read_field(client, 0x90, &inventory, 8) &&
        read_memory(inventory, &signature, 4) && signature == 0x01020304 &&
        read_field(inventory, 0x40, &held, 8) &&
        read_memory(held, fields, sizeof fields) && fields[0] == 4 &&
        client_unit(4,fields[2]) == held) dm_hud_set_held(fields[2]);
    else dm_hud_set_held(0);
}
static void read_belt_portals(uint64_t inventory, dm_hud_info *h) {
    uint64_t unit=0;
    if (!read_field(inventory, 0x10, &unit, 8)) return;
    for (unsigned visited=0; unit && visited<1024; ++visited) {
        uint32_t fields[4], x=0;
        uint64_t data=0, path=0, next=0;
        if (!read_memory(unit, fields, sizeof fields) || fields[0] != 4 ||
            !read_field(unit, 0x10, &data, 8) || !data ||
            !read_field(data, 0xb0, &next, 8) || next == unit) return;
        if (fields[3] == 2 && read_field(unit, 0x38, &path, 8) &&
            read_field(path, 0x10, &x, 4) && x < 16) ++h->belt[x % 4];
        if (fields[1] == 544) ++h->portal;
        if (fields[1] == 533) {
            uint32_t quantity=0;
            if (stat_value(unit, 70, 0, &quantity) && quantity <= 20)
                h->portal += (int)quantity;
        }
        unit=next;
    }
}
static uint64_t hud_tick(uint64_t game) {
    dm_hud_info h={0};
    uint64_t player=controlled_player(game), inventory=0, path=0;
    uint32_t signature=0, mode=0, level=0, xp=0, stats=0, skills=0;
    uint16_t coordinates[4];
    if (!player || !read_field(player, 0xc, &mode, 4) || mode > 20 ||
        !read_field(player, 0x38, &path, 8) ||
        !read_memory(path, coordinates, sizeof coordinates) ||
        (!coordinates[1] && !coordinates[3]) ||
        !read_field(player, 0x90, &inventory, 8) ||
        !read_memory(inventory, &signature, 4) || signature != 0x01020304 ||
        !stat_value(player, 12, 0, &level) || !level || level > 99) {
        dm_hud_publish(&h); dm_hud_set_held(0); return 0;
    }
    h.valid=1; h.level=(int)level; h.players=last_players;
    h.dead=dm_hud_player_mode_dead(mode);
    h.x=coordinates[1]; h.y=coordinates[3];
    uint64_t room=0, area_room=0, area=0; uint32_t number=0;
    if (read_field(path, 0x20, &room, 8) && read_field(room, 0x18, &area_room, 8) &&
        read_field(area_room, 0x90, &area, 8) && read_field(area, 0x1f8, &number, 4) &&
        number < 200) h.area=(int)number;
    stat_value(player, 4, 0, &stats); stat_value(player, 5, 0, &skills);
    h.points = (uint64_t)stats + skills <= INT_MAX ? (int)(stats + skills) : 0;
    int *values[]={&h.hp,&h.hp_max,&h.mp,&h.mp_max};
    for (unsigned i=0; i<4; ++i) {
        uint32_t value=0;
        if (stat_value(player, 6+i, 1, &value) && value <= INT_MAX)
            *values[i]=(int)value / 256;
    }
    stat_value(player, 13, 0, &xp);
    if (level == 99) h.exp_permille=1000;
#ifdef DM_ANDROID_HUD_EXP_HEADER
    else if (k_exp[level] > k_exp[level-1] && xp > k_exp[level-1]) {
        uint64_t progress=(uint64_t)(xp-k_exp[level-1])*1000/(k_exp[level]-k_exp[level-1]);
        h.exp_permille=progress > 1000 ? 1000 : (int)progress;
    }
#else
    (void)xp;
#endif
    read_belt_portals(inventory, &h);
    uint8_t quest=0;
    if (read_field((uint64_t)image->base, QUEST_HINT, &quest, 1)) h.quest_unread=quest != 0;
    publish_held(player);
    dm_hud_publish(&h);
    return player;
}
static void tick_callback(const uint64_t *registers) {
    if (!registers) return;
    pthread_mutex_lock(&lock);
    uint64_t runs=live_runs(), game=registers[0], player=0;
    uint8_t *main_base=image?image->base:NULL;
    int stopped=0, valid=0;
    uint32_t tick=0;
    if (!runs || !read_field(game, 0x170, &tick, 4)) clear_run();
    else {
        if (background_active && (game!=background_game || runs!=background_runs)) clear_freeze();
        if (paused_game && (game!=paused_game || runs!=paused_runs)) paused_game=paused_runs=0;
        last_game=game; last_runs=runs;
        valid=1;
        if (restore_pending) restore_pause_locked();
        apply_save_request(game,runs); player=hud_tick(game);
        panel_tick_locked(game,runs,tick);
        if (frozen_now_locked()) {
            stopped=1;
            player=0; /* Host actions must not advance a frozen world either. */
            frozen_tick_game=game; frozen_tick_runs=runs;
            uint32_t previous=tick-1;
            if (!write_memory(game+0x170,&previous,4)) {
                LOG("ANDROID_D2R_PAUSED_SAVE tick_counter_write_failed=1");
                restore_pause_locked();
            }
        } else frozen_tick_game=frozen_tick_runs=0;
        main_base=image->base;
    }
    pthread_mutex_unlock(&lock);
    /* Rift actions can request another save; never call them under lock. */
    if (player && dm_android_d2r_rift_tick)
        dm_android_d2r_rift_tick(game,player,main_base);
    /* Native callees run without the save mutex, so foreground cancellation
     * never waits for an original game function to finish. */
    if (player && dm_android_d2r_players_get) {
        int players=dm_android_d2r_players_get(main_base);
        if (players<1 || players>8) players=0;
        pthread_mutex_lock(&lock);
        if (last_game==game && last_runs==runs && live_runs()==runs) {
            last_players=players;
            dm_hud_info h; dm_hud_get(&h);
            if (h.valid) { h.players=players; dm_hud_publish(&h); }
        }
        pthread_mutex_unlock(&lock);
    }
    if (!stopped && dm_android_d2r_ui_tick)
        dm_android_d2r_ui_tick(valid?game:0,valid?player:0,main_base);
    /* A debug warp may invalidate guest objects. It must be the final host
     * consumer, outside the save mutex; never pass a frozen world's player. */
    if (dm_android_d2r_debug_tick)
        dm_android_d2r_debug_tick(valid?game:0,player,main_base,valid?runs:0);
}
int dm_android_d2r_save_attach(module_t *main) {
    if (!main || strcmp(main->name, "main") || !main->base ||
        strcmp(main->build_id, D2R_BUILD_ID) ||
        main->size < RUN_SLOTS + sizeof run_slots || !gate_obs_present(OBS_TICK)) {
        LOG("ANDROID_D2R_SAVE_ATTACH disabled (unverified main or missing tick observer)");
        return 0;
    }
    pthread_mutex_lock(&lock);
    if (image) { int same=image == main; pthread_mutex_unlock(&lock); return same; }
    dm_android_save_adapter adapter={.title="d2r-3.3.94012",
        .is_in_game=game_callback, .request_save=save_callback,
        .begin_save_wait=begin_wait, .end_save_wait=end_wait};
    int error=dm_android_save_bind(&adapter);
    if (error) { pthread_mutex_unlock(&lock); return 0; }
    image=main;
    gate_obs_set(OBS_TICK, tick_callback);
    int paused_save=freeze_ready();
    pthread_mutex_unlock(&lock);
    LOG("ANDROID_D2R_SAVE_ATTACH build=%s tick_save_request=1 hud_read_only=1 hud_held=1 hud_experience=%d hud_players=%d paused_save=%d",
        D2R_BUILD_ID, HUD_EXPERIENCE,dm_android_d2r_players_get!=NULL,paused_save);
    return 1;
}
