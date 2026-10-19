#pragma once

#include <stdint.h>
#include "../ios/Sources/dm_touch_controls.h"

/* The Android adapter keeps the same game-facing state contract as the iOS
 * touch front end.  These calls are deliberately independent of JNI so the
 * native game thread can publish game/UI observations without reaching into
 * the Java view. */
typedef struct {
    int navigation;
    int panel_bits;          /* stash=1, vendor=2, death modal=4 */
    int belt_rows;
    int belt_open;
    int holding;
    int pointer_item;        /* dm_tc_pointer_item kind, -1 when unknown */
    int tap_box;
    uint32_t cur_stack;
    uint32_t hover_unid;
    uint32_t target_mask;    /* 0 keeps the mode-derived default */
} dm_android_touch_game_state;

typedef struct {
    uint32_t world_tap_seq;
    float world_x, world_y;  /* Normalized view coordinates [0,1]. */
    uint32_t mouse_click_seq;
    float mouse_x, mouse_y;  /* Normalized view coordinates [0,1]. */
    uint32_t tab_tap_seq, item_tap_seq, long_press_seq;
    uint32_t ident_seq, use_seq;
    int diff_request, belt_request;
    /* Acknowledgements are scoped to the snapshot's cancellation generation
     * and request versions, so a callback cannot clear newer input. */
    uint32_t cancel_generation, diff_request_seq, belt_request_seq;
    uint32_t stack_take_seq;
} dm_android_touch_events;

#if defined(__cplusplus)
static_assert(sizeof(dm_android_touch_game_state) == 40, "Android touch game-state ABI drift");
static_assert(sizeof(dm_android_touch_events) == 68, "Android touch event ABI drift");
#else
_Static_assert(sizeof(dm_android_touch_game_state) == 40, "Android touch game-state ABI drift");
_Static_assert(sizeof(dm_android_touch_events) == 68, "Android touch event ABI drift");
#endif

/* read_state returns non-zero when it filled a valid snapshot; zero leaves the
 * previous snapshot unchanged (useful while the guest is loading). */
typedef int (*dm_android_touch_read_state_fn)(void *opaque, dm_android_touch_game_state *state);
typedef void (*dm_android_touch_dispatch_events_fn)(void *opaque, const dm_android_touch_events *events);

/* Optional game-thread bridge.  Callbacks must be short and nonblocking; they
 * run from the Android HID sampling thread, never from the Java UI thread. */
void dm_android_touch_bind_game_bridge(dm_android_touch_read_state_fn read_state,
                                       dm_android_touch_dispatch_events_fn dispatch_events,
                                       void *opaque);
void dm_android_touch_game_tick(void);

/* Returns the iOS layout target at normalized view coordinates, or -1. */
int dm_android_touch_hit(float x, float y);
void dm_android_touch_set_game_state(const dm_android_touch_game_state *state);
void dm_android_touch_get_game_state(dm_android_touch_game_state *state);
void dm_android_touch_set_target_mask(uint32_t mask);
void dm_android_touch_get_events(dm_android_touch_events *events);
void dm_android_touch_consume_events(const dm_android_touch_events *events);
/* Normalized original-view pointer, sampled without retaining the UI lock. */
int dm_android_touch_pointer(float *x, float *y, int *query_item);
/* UI consumes a lock-protected copy; never inspect controls on another thread. */
int dm_android_touch_visual_state(int *skill_set,int *diff_open,uint64_t *buttons);

/* Native/JNI front ends share these entry points. Coordinates use [0,1]
 * within the resized view; IDs remain stable for the life of each contact. */
void dm_android_touch_resize(float width,float height,float left,float right);
void dm_android_touch_set_enabled(int enabled);
void dm_android_touch_cancel(void);
/* Conditional cancellation is atomic with respect to contact replacement. */
int dm_android_touch_cancel_at_generation(uint32_t expected_generation);
int dm_android_touch_event(const int32_t *ids,const float *xs,const float *ys,
                           int count,int cancelled);
/* Title flow callbacks run with the portable-state lock held. They may only
 * inspect/mutate this state and their own local queues, never call the guest,
 * acquire another subsystem lock, or block. BEGIN/MOVE follow the portable
 * event; RELEASE may defer the physical lift, END/AFTER_END surround the actual
 * portable release (including a release resumed by the game thread). */
enum { DM_ATF_BEGIN, DM_ATF_MOVE, DM_ATF_RELEASE, DM_ATF_END, DM_ATF_AFTER_END, DM_ATF_RESET };
/* RESET's slot identifies expected panel/mask changes versus an explicit
 * lifecycle, resize, disable or user cancellation. No finger uses these IDs. */
enum { DM_ATF_RESET_EXPLICIT=-1, DM_ATF_RESET_SCENE=-2 };
typedef int (*dm_android_touch_flow_fn)(void *,dm_tc_state *,uint32_t,int,int,double);
void dm_android_touch_bind_flow(dm_android_touch_flow_fn callback,void *opaque);
typedef struct {
    dm_tc_state controls;
    uint32_t cancel_generation;
    int ready,enabled;
} dm_android_touch_snapshot;
void dm_android_touch_snapshot_get(dm_android_touch_snapshot *out);
typedef void (*dm_android_touch_edit_fn)(dm_tc_state *,void *);
/* expected_generation is required; callbacks obey the same no-guest rule. */
int dm_android_touch_edit(uint32_t expected_generation,dm_android_touch_edit_fn callback,void *opaque);
/* Resume a deferred release through the normal END/AFTER_END hooks. */
int dm_android_touch_finish_pending(uint32_t expected_generation,int slot,double end_time);
/* Keep the actual final position, including ACTION_UP with no MOVE before it. */
int dm_android_touch_release(int32_t id,float x,float y);
