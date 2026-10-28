/* 镜头拖动使用真实触控状态机，验证默认关闭、两档差异、菜单屏蔽和取消清空。
 * Camera drag exercises the real touch state machine: default off, both modes, menu suppression and cancellation. */
#include "../Sources/dm_touch_controls.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static void drag(dm_tc_state *s,float dx,float dy){
    dm_tc_begin_at(s,0,800,200,1);assert(s->finger[0].target==DM_TC_NONE);
    dm_tc_move_at(s,0,800+dx,200+dy,1.1);
}
int main(void){
    dm_tc_state s;dm_tc_init(&s,1280,720);assert(!s.cam_on);drag(&s,80,-80);assert(!s.cam_yaw&&!s.cam_pitch);dm_tc_cancel_all(&s);
    s.cam_on=1;drag(&s,80,-120);assert(s.cam_pitch>0&&!s.cam_yaw&&s.finger[0].cam_drag);dm_tc_cancel_all(&s);assert(!s.cam_pitch&&!s.cam_yaw);
    s.cam_on=2;drag(&s,80,-80);assert(s.cam_pitch>0&&s.cam_yaw>0&&s.finger[0].cam_drag);dm_tc_end_at(&s,0,1.2);assert(!s.pointer_pulse_pending);
    dm_tc_cancel_all(&s);dm_tc_set_navigation_mode(&s,1);drag(&s,80,-80);assert(!s.cam_pitch&&!s.cam_yaw);
    puts("CAMERA_TOUCH PASS default_off shoulder_pitch firstperson_turn cancel navigation");
}
