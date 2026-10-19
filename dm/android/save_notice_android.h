#pragma once

#include <android/native_activity.h>

/* The filesystem callback never waits for UI. Front ends consume the pending
 * error on their UI thread and leave transaction recovery to the filesystem. */
int dm_android_save_failure_take(void);
void dm_android_save_notice_poll(ANativeActivity *activity);
void dm_android_save_notice_destroy(ANativeActivity *activity);
