/* 相机前端 JNI，沿用本地触控状态与生命周期，不直接读取客体对象。
 * Camera frontend JNI uses local touch state and lifecycle, without reading guest objects directly. */
#include "touch_controls_android.h"
#include "../host/dm_hud.h"
#include <jni.h>
#include <math.h>
int dm_cam_on(void);
void dm_cam_turn(float);
int dm_cam_threats(float *,int);
int dm_cam_map_heading(float *);
int dm_cam_sensor_mode(void);
static int camera_active(void) {
    dm_android_touch_game_state s;dm_android_touch_get_game_state(&s);
    dm_hud_info h;dm_hud_get(&h);
    return h.valid&&!s.navigation&&!(s.panel_bits&4)&&dm_cam_on();
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCamActive(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return camera_active();
}
JNIEXPORT void JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCamTurn(JNIEnv *env,jclass cls,jfloat rad) {
    (void)env;(void)cls;if(isfinite(rad)&&fabsf(rad)<=1&&camera_active())dm_cam_turn(rad);
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCamSensorMode(JNIEnv *env,jclass cls) {
    (void)env;(void)cls;return dm_cam_sensor_mode();
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCamThreats(JNIEnv *env,jclass cls,jfloatArray out) {
    (void)cls;if(!out||!camera_active())return 0;
    int max=(*env)->GetArrayLength(env,out)/5;if(max>32)max=32;
    float data[160];int count=dm_cam_threats(data,max);
    if(count)(*env)->SetFloatArrayRegion(env,out,0,count*5,data);return count;
}
JNIEXPORT jint JNICALL Java_org_d2r_binaryprobe_GameActivity_nativeCamHeading(JNIEnv *env,jclass cls,jfloatArray out) {
    (void)cls;if(!out||(*env)->GetArrayLength(env,out)<4||!camera_active())return 0;
    float data[4];int on=dm_cam_map_heading(data);if(on)(*env)->SetFloatArrayRegion(env,out,0,4,data);return on;
}
