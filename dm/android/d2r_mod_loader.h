#pragma once
#include <stddef.h>
/* Startup only. Empty/missing mods/active.txt keeps the current paths.
 * Invalid selected packages fail closed before mounting a save. */
int dm_android_mod_configure(const char *root,char *overlay,size_t overlay_capacity,
                             char *save,size_t save_capacity);

int dm_android_mod_matches_overlay(const char *package,const char *overlay);
