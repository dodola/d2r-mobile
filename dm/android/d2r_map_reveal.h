#pragma once
#include "../host/dm.h"
enum { DM_MAP_UNAVAILABLE, DM_MAP_IDLE, DM_MAP_QUEUED, DM_MAP_RUNNING,
       DM_MAP_DONE, DM_MAP_CANCELLED, DM_MAP_FAILED };
typedef struct {
    uint64_t sequence;
    uint32_t state,area,total,done,ready;
    int error;
} dm_android_map_status;
int dm_android_map_attach(module_t *main);
/* UI/server callers only enqueue; all client guest calls belong to pump. */
int dm_android_map_request(uint64_t *sequence);
void dm_android_map_status_get(dm_android_map_status *out);
void dm_android_map_pump(void);
