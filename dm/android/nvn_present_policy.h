#pragma once
#include <stdint.h>

/* Display-aware base rate (from android-integration 0cf4904, made panel-safe).
 * The guest asks for interval 2, which is 33.3 ms on a 60 Hz base. When the
 * window really runs a 120 Hz mode, a base of 80 gives 25 ms, exactly three
 * vsyncs, so a GPU that can beat 33 ms is no longer capped at 30 FPS. Any
 * other mode keeps 60: 25 ms on a 60 Hz panel would alternate 16.7/33.3 ms. */
static inline int dm_android_cadence_base_hz(int display_hz, int disabled) {
    return !disabled && display_hz==120 ? 80 : 60;
}

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
