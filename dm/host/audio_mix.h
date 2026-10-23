#pragma once
#include <stdint.h>

/* Switch 5.1 PCM order: FL, FR, centre (NPC dialogue), LFE, SL, SR.
 * ITU-R BS.775 Lo/Ro, with 3 dB of headroom and saturation. LFE is omitted.
 * Q15: 0.707 = 23170, 0.707 * 0.707 = 16384. */
static inline void downmix51(const int16_t *in, int16_t *out, unsigned frames) {
    for (unsigned i = 0; i < frames; ++i, in += 6, out += 2) {
        int32_t l = (23170 * in[0] + 16384 * in[2] + 16384 * in[4]) >> 15;
        int32_t r = (23170 * in[1] + 16384 * in[2] + 16384 * in[5]) >> 15;
        out[0] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
}
