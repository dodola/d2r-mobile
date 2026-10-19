#pragma once
#include <stdint.h>

/* Frame completion here means mtl_present returned after real FIFO acquire /
 * queue-present work. It is not a physical scanout-completion timestamp. */
void dm_android_scale3d_present(uint64_t presented_ns,uint64_t target_period_ns);
void dm_android_scale3d_thermal(int status);
typedef struct {
    int supported,quality,level,max_level,thermal_status,disabled;
    float configured,current,floor;
    uint32_t internal_width,internal_height,output_width,output_height;
    uint64_t samples,changes,hold_until,target_period_ns,last_present_ns,last_caller;
    double interval_ms,drop_rate_10s,drop_rate_60s;
} dm_android_scale3d_status;
void dm_android_scale3d_status_get(dm_android_scale3d_status *out);
