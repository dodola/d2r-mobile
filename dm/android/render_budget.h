#pragma once
#include <math.h>
#include <stdint.h>

/* Match the actual landscape viewport without raising the existing vertical
 * workload. Width is aligned to eight pixels like the shared iOS profile.
 * Narrow windows retain the guest UI's 16:9 minimum in a fitted viewport. */
static inline uint64_t dm_android_display_resolution(unsigned w,unsigned h,int docked) {
    unsigned height=docked?900u:720u;
    uint64_t width=docked?1600u:1280u;
    if(w && h && (uint64_t)w*9>(uint64_t)h*16) {
        width=(((uint64_t)w*height+(uint64_t)h*4)/((uint64_t)h*8))*8;
        if(width>8192)width=8192;
    }
    return ((uint64_t)height<<32)|width;
}

/* vickwv ef8b1ff/312c2f8: vertical output tiers and absolute world pixels. */
static inline uint64_t dm_android_tier_resolution(unsigned w,unsigned h,int tier) {
    if(!w||!h||tier<0||tier>2)return 0;
    unsigned target=tier==0?800u:tier==1?1080u:h;
    if(target>h)target=h;
    unsigned width=(unsigned)(((uint64_t)w*target+h/2)/h);
    if(!width)width=1;
    return ((uint64_t)target<<32)|width;
}
static inline float dm_android_world_scale(unsigned w,unsigned h,int tier) {
    if(!w||!h||tier<0||tier>2)return .75f;
    double budget=tier==0?850000.:tier==1?1100000.:1750000.;
    double scale=sqrt(budget/((double)w*h));
    return (float)(scale<.70?.70:scale>1.?1.:scale);
}
