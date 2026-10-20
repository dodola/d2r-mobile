#pragma once
#include "../host/dm.h"
/* Startup only. Selects debug/save when explicitly enabled; otherwise leaves
 * the caller's normal save path untouched. Returns errno on failure. */
int dm_android_d2r_debug_configure(const char *root,char *save,size_t capacity);
int dm_android_d2r_debug_attach(module_t *main);
/* Original server tick only, after all other host gameplay consumers. */
void dm_android_d2r_debug_tick(uint64_t game,uint64_t player,uint8_t *base,uint64_t runs);
