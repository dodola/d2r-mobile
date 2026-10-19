#pragma once
#include <stdint.h>
void dm_android_ar_suspend(int suspended);
typedef struct {
    uint64_t updates,rendered_blocks,written_frames,device_read_frames,nonzero_samples,sink_commands,completion_events;
    int open_renderers,started_renderers,last_error;
} dm_android_ar_metrics;
void dm_android_ar_snapshot(dm_android_ar_metrics *metrics);
