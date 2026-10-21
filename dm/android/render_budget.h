#pragma once
#include <math.h>
#include <stdint.h>

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
