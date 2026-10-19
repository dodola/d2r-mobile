#pragma once
#include "../host/dm.h"

int dm_android_custom_attach(module_t *main);
int dm_android_title_custom_settings(void);
int dm_android_d2r_fps_setting(void);
/* D2R-only shell startup: consume explicit language before platform binding.
 * NULL means leave the Activity's actual system locale unchanged. */
const char *dm_android_custom_startup_locale(void);
/* Select one complete overlay root. Never combine a variant index with a
 * different archive. Returns 1 when a usable root was copied to out. */
int dm_android_custom_overlay(const char *port_root,char *out,size_t capacity);
/* Only the original game input callback enters this scope around actions.
 * UI/JNI threads have no scope and cannot access original widgets. */
void dm_android_custom_scope(int enter);
