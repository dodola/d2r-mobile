#pragma once
#include <stdint.h>

/* Reviewed D2R 3.3.94012 host adapters; linked only after exact title identity checks. */
int dm_android_d2r_host_attach(void);
int dm_android_title_host_call(int number, uint64_t *registers);

typedef struct {
    int attached, game, navigation;
    uint64_t panels, scene_epoch, world_epoch, uimsg_count;
} dm_android_d2r_scene_status;
/* Copies message-derived state; never calls the original game or reads its heap. */
void dm_android_d2r_scene_status_get(dm_android_d2r_scene_status *out);
