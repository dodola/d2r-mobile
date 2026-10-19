#pragma once
#include "touch_controls_android.h"

/* Event/edit callbacks are pure and run under the Android controls lock.
 * The pump and classification completion run only on the game HID thread. */
int dm_android_skill_flow_event(dm_tc_state *,uint32_t generation,int slot,int phase,double now);
void dm_android_skill_flow_classified(uint32_t generation,int slot,double contact_start);
void dm_android_skill_flow_pump(void);
