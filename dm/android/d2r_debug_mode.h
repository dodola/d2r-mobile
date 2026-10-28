#pragma once
#include "../host/dm.h"
/* Startup only. Selects debug/save when explicitly enabled; otherwise leaves
 * the caller's normal save path untouched. Returns errno on failure. */
int dm_android_d2r_debug_configure(const char *root,char *save,size_t capacity);
int dm_android_d2r_debug_attach(module_t *main);
/* Original server tick only, after all other host gameplay consumers. */
void dm_android_d2r_debug_tick(uint64_t game,uint64_t player,uint8_t *base,uint64_t runs);
/* Native game widgets enqueue; only the server tick may execute mutations. */
int dm_android_debug_submit(const char *command);
int dm_android_debug_active(void);
int dm_android_debug_next_boot(int enabled);
int dm_android_debug_requested(void);
void dm_android_debug_summary(char *text,size_t capacity);
void dm_android_debug_ui_message(const char *,const char *,const char *);
void dm_android_debug_ui_pump(uint8_t *base);

int dm_android_debug_ui_touch(int phase,float x,float y);
int dm_android_debug_ui_is_open(void);
