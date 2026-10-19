#pragma once
#include <stdint.h>

/* The explicit 60 FPS experiment ignores the guest's present interval, but
 * remains capped. Otherwise preserve the original Android cadence exactly,
 * including its integer rounding and the headless/test hz=0 escape hatch. */
static inline uint64_t dm_android_present_period_ns(int base_hz,
        int apply_guest_interval, int guest_interval, int fixed_60) {
    if(fixed_60)return 1000000000ULL/60;
    if(!base_hz)return 0;
    uint64_t interval=guest_interval>0?(uint64_t)guest_interval:1;
    return (1000000000ULL/(uint64_t)base_hz)*(apply_guest_interval?interval:1);
}

/* Power saving is a cap, so a slower guest/GPU never runs faster when it is
 * enabled. An explicit uncapped diagnostic stays uncapped. */
static inline uint64_t dm_android_present_power_period_ns(uint64_t guest_period,
        int eco_fps) {
    const uint64_t eco_period=1000000000ULL/30;
    return guest_period && eco_fps && guest_period<eco_period ? eco_period : guest_period;
}
