#pragma once
#include "touch_controls_android.h"
void dm_android_d2r_touch_shell_event(dm_tc_state *,uint32_t,int,int,double);
/* Verified title/game thread only; callbacks and edits never read the guest. */
void dm_android_d2r_touch_shell_pump(void);
