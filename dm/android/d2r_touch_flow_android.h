#pragma once
#include "touch_controls_android.h"

/* Event hook only records local state. The pump is called exclusively by the
 * verified title's HID callback, where the shared cursor code may read UI. */
int dm_android_d2r_touch_flow_event(dm_tc_state *controls,uint32_t generation,
                                   int slot,int phase,double now);
void dm_android_d2r_touch_flow_pump(void);
