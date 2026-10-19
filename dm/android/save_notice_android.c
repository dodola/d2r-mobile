#include "save_notice_android.h"
#include "../host/dm.h"
#include "../host/dm_i18n.h"
#include "../host/front.h"
#include <errno.h>
#include <stdatomic.h>

static _Atomic int pending_error;
/* NativeActivity calls these helpers only on its main Looper thread. */
static jobject native_dialog;

void front_save_failed(int err) {
    int empty = 0;
    atomic_compare_exchange_strong(&pending_error, &empty, err > 0 ? err : EIO);
    LOG("ANDROID_SAVE_FAILED errno=%d notification=queued", err);
}

int dm_android_save_failure_take(void) { return atomic_exchange(&pending_error, 0); }

static int clear_exception(JNIEnv *env) {
    if (!(*env)->ExceptionCheck(env)) return 0;
    (*env)->ExceptionClear(env);
    return 1;
}

void dm_android_save_notice_destroy(ANativeActivity *activity) {
    if (!native_dialog) return;
    JNIEnv *env = activity->env;
    jclass cls = (*env)->GetObjectClass(env, native_dialog);
    jmethodID dismiss = cls ? (*env)->GetMethodID(env, cls, "dismiss", "()V") : NULL;
    if (dismiss) (*env)->CallVoidMethod(env, native_dialog, dismiss);
    clear_exception(env);
    if (cls) (*env)->DeleteLocalRef(env, cls);
    (*env)->DeleteGlobalRef(env, native_dialog);
    native_dialog = NULL;
}

void dm_android_save_notice_poll(ANativeActivity *activity) {
    JNIEnv *env = activity->env;
    if (native_dialog) {
        jclass cls = (*env)->GetObjectClass(env, native_dialog);
        jmethodID showing = cls ? (*env)->GetMethodID(env, cls, "isShowing", "()Z") : NULL;
        jboolean visible = showing ? (*env)->CallBooleanMethod(env, native_dialog, showing) : JNI_FALSE;
        int error = clear_exception(env);
        if (cls) (*env)->DeleteLocalRef(env, cls);
        if (visible && !error) return;
        (*env)->DeleteGlobalRef(env, native_dialog); native_dialog = NULL;
    }
    int err = dm_android_save_failure_take();
    if (!err) return;
    if ((*env)->PushLocalFrame(env, 16) < 0) { clear_exception(env); front_save_failed(err); return; }
    jclass cls = (*env)->FindClass(env, "android/app/AlertDialog$Builder");
    jmethodID init = cls ? (*env)->GetMethodID(env, cls, "<init>", "(Landroid/content/Context;)V") : NULL;
    jmethodID title = init ? (*env)->GetMethodID(env, cls, "setTitle", "(Ljava/lang/CharSequence;)Landroid/app/AlertDialog$Builder;") : NULL;
    jmethodID button = title ? (*env)->GetMethodID(env, cls, "setPositiveButton", "(Ljava/lang/CharSequence;Landroid/content/DialogInterface$OnClickListener;)Landroid/app/AlertDialog$Builder;") : NULL;
    jmethodID show = button ? (*env)->GetMethodID(env, cls, "show", "()Landroid/app/AlertDialog;") : NULL;
    jobject builder = show ? (*env)->NewObject(env, cls, init, activity->clazz) : NULL;
    if (builder && !(*env)->ExceptionCheck(env)) {
        jstring message = (*env)->NewStringUTF(env, dm_ui("dmSaveNoSpace"));
        jstring confirm = (*env)->NewStringUTF(env, dm_ui("dmConfirm"));
        if (message && confirm && !(*env)->ExceptionCheck(env)) {
            (*env)->CallObjectMethod(env, builder, title, message);
            if (!(*env)->ExceptionCheck(env)) (*env)->CallObjectMethod(env, builder, button, confirm, NULL);
            jobject dialog = !(*env)->ExceptionCheck(env) ? (*env)->CallObjectMethod(env, builder, show) : NULL;
            if (dialog && !(*env)->ExceptionCheck(env)) native_dialog = (*env)->NewGlobalRef(env, dialog);
        }
    }
    int failed = clear_exception(env) || !native_dialog;
    (*env)->PopLocalFrame(env, NULL);
    if (failed) front_save_failed(err);
    else LOG("ANDROID_SAVE_FAILED notification=shown errno=%d frontend=NativeActivity", err);
}

JNIEXPORT jstring JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeSaveFailure(JNIEnv *env, jclass cls) {
    (void)cls;
    int err = dm_android_save_failure_take();
    if (!err) return NULL;
    jstring text = (*env)->NewStringUTF(env, dm_ui("dmSaveNoSpace"));
    if (!text) front_save_failed(err);
    else LOG("ANDROID_SAVE_FAILED notification=delivered errno=%d frontend=Activity", err);
    return text;
}
