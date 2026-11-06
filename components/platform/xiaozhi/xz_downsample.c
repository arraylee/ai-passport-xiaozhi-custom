#include "xz_downsample.h"
#include <stddef.h>
#include <string.h>
/* 24-tap Hamming-windowed sinc, 3.2kHz cutoff, unit DC gain Q15.
 * Generated deterministically from 2*fc*sinc(2*fc*(n-11.5))*Hamming(n).
 * Both microphone and hardware reference use the identical filter. */
static const int16_t FIR_16000[24] = {69, 57, -95, -263, 0, 679, 635, -943, -2276, 0, 6344, 12177, 12177, 6344, 0, -2276, -943, 635, 679, 0, -263, -95, 57, 69};
static const int16_t FIR_24000[24] = {-15, 57, 160, 206, 0, -532, -1076, -945, 498, 3237, 6354, 8440, 8440, 6354, 3237, 498, -945, -1076, -532, 0, 206, 160, 57, -15};

bool xz_downsample_init(xz_downsample_t *s, unsigned rate) {
    if (!s) return false;
    memset(s, 0, sizeof *s);
    if (rate != 16000 && rate != 24000) return false;
    s->factor = rate / 8000;
    return true;
}
static int16_t filter(const int16_t *history, unsigned at, const int16_t *coeff) {
    int64_t total = 0;
    for (unsigned i = 0; i < 24; ++i)
        total += (int32_t)history[(at + 24 - i) % 24] * coeff[i];
    total /= 32768;
    return total > 32767 ? 32767 : total < -32768 ? -32768 : (int16_t)total;
}
bool xz_downsample_push(xz_downsample_t *s, int16_t mic, int16_t reference,
                        int16_t *out_mic, int16_t *out_ref) {
    if (!s || !out_mic || !out_ref || (s->factor != 2 && s->factor != 3)) return false;
    unsigned at = s->at;
    s->history[0][at] = mic; s->history[1][at] = reference;
    s->at = (at + 1) % 24;
    if (++s->phase < s->factor) return false;
    s->phase = 0;
    const int16_t *coeff = s->factor == 2 ? FIR_16000 : FIR_24000;
    *out_mic = filter(s->history[0], at, coeff);
    *out_ref = filter(s->history[1], at, coeff);
    return true;
}
