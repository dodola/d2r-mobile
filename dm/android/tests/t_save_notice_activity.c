/* Isolated UI fixture for the production NativeActivity save notification.
 * It includes no guest image, game assets or save mount.  Its only file is a
 * diagnostic receipt in this fixture package's own internal directory. */
#include "../save_notice_android.h"
#include "../../host/front.h"
#include <android/log.h>
#include <errno.h>
#include <stdio.h>

static int injected;

static void focus_changed(ANativeActivity *activity, int focused) {
    if (!focused) return;
    if (!injected) {
        injected = 1;
        front_save_failed(ENOSPC);
        front_save_failed(EIO); /* A second failure must not stack dialogs. */
        __android_log_print(ANDROID_LOG_INFO, "dm_save_notice_probe", "injected ENOSPC and EIO");
    }
    dm_android_save_notice_poll(activity);
}

static void destroyed(ANativeActivity *activity) {
    dm_android_save_notice_destroy(activity);
    char path[1024];
    snprintf(path, sizeof path, "%s/cleanup.json", activity->internalDataPath);
    FILE *file = fopen(path, "w");
    if (file) { fputs("{\"dialog_cleanup_completed\":true}\n", file); fclose(file); }
    __android_log_print(ANDROID_LOG_INFO, "dm_save_notice_probe", "destroyed with dialog cleanup");
}

static void paused(ANativeActivity *activity) { ANativeActivity_finish(activity); }

void ANativeActivity_onCreate(ANativeActivity *activity, void *saved, size_t size) {
    (void)saved; (void)size;
    injected = 0;
    activity->callbacks->onWindowFocusChanged = focus_changed;
    activity->callbacks->onPause = paused;
    activity->callbacks->onDestroy = destroyed;
}
