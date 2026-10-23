#include "../render_budget.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    const unsigned screens[][2]={{2800,1264},{2665,1264},{2624,1220},{2400,1080},{1920,1080},{2560,1600},{1,1},{0,720},{2800,0}};
    unsigned checks=0;
    for(unsigned n=0;n<sizeof screens/sizeof *screens;n++)for(int docked=0;docked<2;docked++) {
        unsigned sw=screens[n][0],sh=screens[n][1],h=docked?900:720;
        uint64_t r=dm_android_display_resolution(sw,sh,docked);unsigned w=(unsigned)r;
        assert((r>>32)==h);assert(w%8==0);assert(w>=(docked?1600u:1280u));checks+=3;
        if(sw && sh && (uint64_t)sw*9>(uint64_t)sh*16) {
            /* Quantization costs at most four guest pixels of horizontal FOV. */
            assert(fabs((double)w-(double)sw*h/sh)<=4);checks++;
        }
    }
    uint64_t ppg=dm_android_display_resolution(2800,1264,0);
    assert((unsigned)ppg==1592 && (ppg>>32)==720);checks++;
    assert((unsigned)dm_android_display_resolution(UINT_MAX,1,0)==8192);checks++;
    assert((unsigned)dm_android_display_resolution(1,UINT_MAX,0)==1280);checks++;
    assert((unsigned)dm_android_tier_resolution(2800,1264,0)==1772);checks++;
    printf("ANDROID_DISPLAY_GEOMETRY checks=%u adaptive_aspect=1 vertical_budget=720 narrow_floor=1 passed=1\n",checks);
    return 0;
}
