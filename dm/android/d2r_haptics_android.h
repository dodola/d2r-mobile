#pragma once

/* Consume one coalesced drop event: 1 heavy, 2 medium, 3 light, 0 none.
 * -1 means playback is disabled. Pending events are drained in all cases. */
int dm_android_drop_haptic_poll(int active);
