#pragma once
#include "../host/dm.h"

/* D2R 3.3.94012 only. The loaded artifact must carry the reviewed build ID
 * and server-tick observer before any private address is inspected. */
int dm_android_d2r_save_attach(module_t *main);
/* Reviewed server-state queries shared by the upstream actions/world code.
 * They reject a stale run cohort before inspecting any server unit. */
uint32_t dm_cur_tick(void);
int dm_diag_difficulty(void);
uint64_t dm_diag_srv_item(uint32_t id);
int dm_diag_srv_merc(uint32_t id,int *hp,int *maximum,int *healing);
void dm_freeze_frame(int want);
int dm_android_d2r_server_context(uint64_t *game,uint64_t *player);
