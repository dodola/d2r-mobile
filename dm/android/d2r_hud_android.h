#pragma once
#include <stdint.h>
#include <stddef.h>
#include "../ios/Sources/dm_touch_game_state.h"

enum { DM_AHUD_VERSION=2, DM_AHUD_HEAD=28, DM_AHUD_GLYPH=12,
       DM_AHUD_KEYS=5, DM_AHUD_PORTAL=88, DM_AHUD_NAVIGATION=100,
       DM_AHUD_DIFF_OPEN=101, DM_AHUD_MAP_CANCEL=102, DM_AHUD_SWAP_HELD=103,
       DM_AHUD_TARGETS=104, DM_AHUD_JOY_X=105, DM_AHUD_JOY_Y=106, DM_AHUD_WORDS=107 };
/* Versioned copy-only ABI, mirrored by D2rHudRenderer. Word 18 is dm_lang_zh().
 * Metadata 20..25 binds
 * a draw acknowledgement to the native HUD identity and scene/weapon bank. */
/* Schema 2 appends a portal glyph and copied control state. Glyph word 11
 * packs uint32 kind and int32 charges; stick origins use 1/1000 point units.
 * Only the five native skill keys participate in render acknowledgements. */
void dm_android_hud_frame(int64_t out[DM_AHUD_WORDS]);
void dm_android_hud_rendered(const int64_t *words,size_t count,uint32_t drawn);
int dm_android_hud_receipt_allows(const dm_tg_snapshot *snapshot,unsigned slot,uint64_t now);
int dm_android_hud_hide_glyph(uint64_t widget);
void dm_android_hud_message(const char *action,const char *argument);
