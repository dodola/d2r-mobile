#include "d2r_haptics_android.h"
#include "../host/dm_drophaptic.h"
#include <jni.h>

int dm_android_drop_haptic_poll(int active) {
    /* The same atomic take used by the iOS frame loop owns deduplication and
     * selects the strongest pending event. Drain even while disabled so a
     * paused, unfocused or disabled frontend cannot replay an old drop. */
    int tier = dm_drop_haptic_take();
    if (!active || !dm_drop_haptic_enabled()) return -1;
    return tier >= 1 && tier <= 3 ? tier : 0;
}

JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameHaptics_nativePoll(
    JNIEnv *env, jclass cls, jboolean active) {
    (void)env; (void)cls;
    return dm_android_drop_haptic_poll(active != JNI_FALSE);
}
