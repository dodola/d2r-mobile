/* 256 字节页的区间位图操作：批量处理整字，保持逐页实现的相同边界和命中数。
 * Range operations for 256-byte page bitmaps: batch full words with exactly the same bounds and hit counts as per-page loops.
 */
#ifndef NVN_VK_SLOT_PAGES_H
#define NVN_VK_SLOT_PAGES_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
static inline void nvn_slot_pages_mark(uint32_t *bits,uint64_t first,uint64_t last) {
    if(first>last)return;
    uint64_t a=first>>5,b=last>>5;uint32_t lo=UINT32_MAX<<(first&31),hi=UINT32_MAX>>(31-(last&31));
    if(a==b){bits[a]|=lo&hi;return;}
    bits[a]|=lo;if(b>a+1)memset(bits+a+1,0xff,(size_t)(b-a-1)*sizeof(*bits));bits[b]|=hi;
}
static inline uint64_t nvn_slot_pages_hits(const uint32_t *reads,const uint32_t *writes,uint64_t first,uint64_t last,int *write_hit) {
    if(first>last)return 0;
    uint64_t count=0,a=first>>5,b=last>>5;
    for(uint64_t i=a;i<=b;i++){
        uint32_t mask=UINT32_MAX;if(i==a)mask&=UINT32_MAX<<(first&31);if(i==b)mask&=UINT32_MAX>>(31-(last&31));
        uint32_t w=writes[i]&mask,r=reads?reads[i]&mask:0;
        if(w)*write_hit=1;
        count+=(unsigned)__builtin_popcount(w|r);
    }
    return count;
}
#endif
