#pragma once
#include "../host/dm.h"
#include "touch_controls_android.h"

int dm_android_d2r_ui_attach(module_t *main);
void dm_android_d2r_ui_pump(dm_android_touch_game_state *state);
void dm_android_d2r_ui_events(void *opaque,const dm_android_touch_events *events);
int dm_android_d2r_frontend_touch(int phase,float x,float y);
void dm_android_d2r_ui_message(const char *category,const char *action,const char *argument);
void dm_android_d2r_ui_tick(uint64_t game,uint64_t player,uint8_t *main);
void dm_android_d2r_ui_c2s(uint64_t game,uint64_t player,const uint8_t *packet,uint32_t length);
