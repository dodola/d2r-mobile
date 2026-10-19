#pragma once
#include <stdint.h>
#include "touch_controls_android.h"

/* A separate copy-only ABI: the UI never calls host getters or dereferences
 * guest objects. All values/text are sampled on the original input thread. */
enum { DM_AWORLD_VERSION=1, DM_AWORLD_HEAD=32, DM_AWORLD_GOALS=12,
       DM_AWORLD_WORDS=DM_AWORLD_HEAD+DM_AWORLD_GOALS*3,
       DM_AWORLD_TEXT_BYTES=256, DM_AWORLD_RIFT=1, DM_AWORLD_MAP=2 };
#define DM_AWORLD_MAX_AGE_NS 500000000ULL
typedef struct {
    int64_t word[DM_AWORLD_WORDS];
    char text[4][DM_AWORLD_TEXT_BYTES]; /* title, subtitle, timer, objective */
} dm_android_world_overlay_snapshot;
_Static_assert(sizeof(dm_android_world_overlay_snapshot)==1568,"world overlay ABI drift");
void dm_android_world_overlay_tick(const dm_android_touch_game_state *state);
void dm_android_world_overlay_message(const char *action,const char *argument);
int dm_android_world_overlay_frame(dm_android_world_overlay_snapshot *out);
