/* 相机输入、切档矩阵失效和并发俯仰，直接调用生产宿主代码，无游戏数据。
 * Camera input, matrix invalidation on mode changes and concurrent pitch use the production host code without game data. */
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include "../dm_cam.c"
void dm_log(const char *fmt,...){(void)fmt;}
static void *pitch(void *unused){(void)unused;for(int i=0;i<100;i++)dm_cam_tilt(0.0001f);return NULL;}
int main(void){
    unsetenv("DM_CAM");assert(!dm_cam_mode());dm_cam_turn(0.2f);dm_cam_tilt(0.2f);assert(!dm_cam_take_turn()&&!dm_cam_tilt_get());
    dm_cam_set_mode(1);float matrix[16]={0};for(int i=0;i<16;i+=5)matrix[i]=1;
    dm_cam_publish(matrix,matrix,matrix,matrix,0,0,1);assert(dm_cam_on());
    dm_cam_turn(NAN);dm_cam_turn(INFINITY);assert(!dm_cam_take_turn());dm_cam_turn(.25f);assert(dm_cam_take_turn()==.25f&&!dm_cam_take_turn());
    pthread_t threads[4];for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,pitch,NULL));for(int i=0;i<4;i++)pthread_join(threads[i],NULL);
    assert(fabsf(dm_cam_tilt_get()-.04f)<.00001f);dm_cam_tilt_clamp(-.01f,.01f);assert(dm_cam_tilt_get()==.01f);
    dm_cam_turn(.3f);dm_cam_set_mode(2);assert(!dm_cam_on()&&!dm_cam_take_turn());
    dm_cam_publish(matrix,matrix,matrix,matrix,0,0,1);assert(!dm_cam_on());
    dm_cam_publish(matrix,matrix,matrix,matrix,0,0,2);assert(dm_cam_on());
    dm_cam_set_mode(0);assert(!dm_cam_on());dm_cam_set_mode(2);assert(!dm_cam_on());dm_cam_set_mode(99);assert(!dm_cam_mode());
    puts("CAM_STATE PASS defaults finite_input concurrent_pitch mode_invalidation stale_publish");
}
