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
/* 按钮消息只排队；游戏线程校验页内控件后消费。
 * Button messages only queue requests, consumed after game-thread validation of inline widgets. */
void dm_android_custom_message(const char *category,const char *action,const char *argument);
void dm_android_custom_settings_init(const char *dir);
int dm_android_custom_mod_count(void);
void dm_android_custom_settings_clear_error(void);
void dm_android_custom_settings_load(int *mode,int *clear,int *store,int *mod);
int dm_android_custom_settings_apply(int mode,int clear,int store,int mod);
void dm_android_custom_status(int mod,int selected,char *out,size_t capacity);
