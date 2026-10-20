#ifndef DM_YIELD_BACKOFF_H
#define DM_YIELD_BACKOFF_H
#include <stdint.h>
typedef struct { uint64_t window;unsigned count;int initialized; } DmYieldBackoff;
/* Per-thread scheduling policy, adapted from vickwv e735628. A new 16 ms
 * window permits 16 ordinary yields before a 100 us sleep is requested. */
static inline int dm_yield_backoff_due(DmYieldBackoff *state,uint64_t now) {
    if(!state->initialized||now-state->window>=16000000ULL){
        state->window=now;state->count=0;state->initialized=1;
    }
    if(state->count<17)state->count++;
    return state->count>16;
}
#endif
