#include "../nvn_present_policy.h"
#include "../../gfx/nvn/nvn_pace.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    unsigned checks=0;
    /* Every legacy combination keeps exactly the same rounding and interval
     * behavior. The fixed mode must stay 60 Hz even when the guest asks for
     * 30/20 FPS, without turning the uncapped escape hatch into the default. */
    for(int hz=0;hz<=1000;hz++)for(int display=0;display<=1;display++)
        for(int interval=0;interval<=8;interval++) {
            uint64_t old=hz?(1000000000ULL/(unsigned)hz)*(display&&interval>0?(unsigned)interval:1):0;
            assert(dm_android_present_period_ns(hz,display,interval,0)==old);
            assert(dm_android_present_period_ns(hz,display,interval,1)==1000000000ULL/60);
            checks+=2;
        }
    assert(dm_android_present_period_ns(60,1,INT_MAX,0)==(1000000000ULL/60)*INT_MAX);
    uint64_t period=dm_android_present_period_ns(60,1,2,1),now=1000000000ULL;
    nvn_pacer pace={0};
    assert(nvn_pace(&pace,now,period)==0);
    uint64_t first=pace.target;
    for(unsigned frame=1;frame<=600;frame++) {
        now+=1000000;
        uint64_t wait=nvn_pace(&pace,now,period);
        assert(wait>0&&wait<period);
        now+=wait;
        assert(now==first+frame*period);
        checks+=2;
    }
    /* A large shader compilation stall starts a new cadence rather than
     * generating a burst of presents. The next fast frame is paced again. */
    now+=200000000;
    assert(nvn_pace(&pace,now,period)==0&&pace.target==now);
    assert(nvn_pace(&pace,now+1000000,period)==period-1000000);
    printf("ANDROID_PRESENT_POLICY checks=%u legacy_equal=1 fixed60=1 paced_frames=600 stall_recovery=1 passed=1\n",checks+4);
    return 0;
}
