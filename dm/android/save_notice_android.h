#pragma once

#ifdef DM_ANDROID_HOST_FS_TEST
/* The Linux FS fixture exercises the real error recorder and take operation;
 * it has no JNI/Activity and never calls the notification UI. */
typedef struct ANativeActivity ANativeActivity;
#else
#include <android/native_activity.h>
#endif

/* The filesystem callback never waits for UI. Front ends consume the pending
 * error on their UI thread and leave transaction recovery to the filesystem. */
int dm_android_save_failure_take(void);
void dm_android_save_notice_poll(ANativeActivity *activity);
void dm_android_save_notice_destroy(ANativeActivity *activity);
