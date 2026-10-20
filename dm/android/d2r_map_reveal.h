#pragma once
#include "../host/dm.h"
enum { DM_MAP_UNAVAILABLE, DM_MAP_IDLE, DM_MAP_QUEUED, DM_MAP_RUNNING,
       DM_MAP_DONE, DM_MAP_CANCELLED, DM_MAP_FAILED, DM_MAP_WAITING };
typedef struct {
    uint64_t sequence;
    uint32_t state,area,total,done,ready;
    int error;
    uint32_t game;
} dm_android_map_status;
int dm_android_map_attach(module_t *main);
/* UI/server callers only enqueue; all client guest calls belong to pump. */
int dm_android_map_request(uint64_t *sequence);
/* 选项页只排队，回到同一区域且解除暂停后才展开。
 * Options only enqueue; reveal after returning to the same area and unpausing. */
int dm_android_map_request_when_ready(uint64_t *sequence);
void dm_android_map_status_get(dm_android_map_status *out);
void dm_android_map_pump(void);
