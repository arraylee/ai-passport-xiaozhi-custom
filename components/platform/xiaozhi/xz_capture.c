#include "xz_capture.h"
#include "xz_bargein.h"
#include "xz_downsample.h"
#include <stdlib.h>
#include <string.h>
struct xz_capture {
    xz_bargein_t *detector;
    xz_downsample_t downsample;
    int16_t mic[XZ_ECHO_FRAME], reference[XZ_ECHO_FRAME];
    unsigned at;
    uint32_t reported_frames;
    unsigned gaps;
};
xz_capture_t *xz_capture_create(void) {
    xz_bargein_t *b = xz_bargein_create(0);
    if (!b) return NULL;
    xz_capture_t *c = calloc(1, sizeof *c);
    if (!c) { xz_bargein_destroy(b); return NULL; }
    c->detector = b;
    return c;
}
bool xz_capture_begin(xz_capture_t *c, unsigned rate) {
    if (!c || !xz_downsample_init(&c->downsample, rate)) return false;
    c->at = 0;
    c->reported_frames = 0;
    c->gaps = 0;
    memset(c->mic, 0, sizeof c->mic);
    memset(c->reference, 0, sizeof c->reference);
    return xz_bargein_resume(c->detector);
}
bool xz_capture_feed(xz_capture_t *c, const int16_t *stereo, size_t frames) {
    if (!c || !stereo || !c->downsample.factor) return false;
    for (size_t i = 0; i < frames; ++i) {
        if (!xz_downsample_push(&c->downsample, stereo[2*i], stereo[2*i+1],
                                &c->mic[c->at], &c->reference[c->at])) continue;
        if (++c->at == XZ_ECHO_FRAME) {
            if (!xz_bargein_feed(c->detector, c->reference, c->mic)) return false;
            c->at = 0;
        }
    }
    return true;
}
bool xz_capture_triggered(const xz_capture_t *c) {
    return c && xz_bargein_triggered(c->detector);
}
bool xz_capture_discontinuity(xz_capture_t *c) {
    if (!c || !c->downsample.factor ||
        !xz_bargein_discontinuity(c->detector)) return false;
    if (!xz_downsample_init(&c->downsample, c->downsample.factor * 8000)) return false;
    c->at = c->reported_frames = 0;
    memset(c->mic, 0, sizeof c->mic);
    memset(c->reference, 0, sizeof c->reference);
    if (c->gaps < UINT32_MAX) ++c->gaps;
    return true;
}
unsigned xz_capture_gap_count(const xz_capture_t *c) { return c ? c->gaps : 0; }
bool xz_capture_observe(xz_capture_t *c, xz_bargein_metrics_t *out) {
    if (!c || !out || !xz_bargein_metrics(c->detector, out) ||
        out->frames == c->reported_frames) return false;
    // Include startup and the exact trigger; a false onset can happen before
    // the first one-second report. These are numeric aggregates, never PCM.
    if (out->frames - c->reported_frames < 125 &&
        !(c->reported_frames == 0 && out->frames >= 16) &&
        !xz_capture_triggered(c)) return false;
    c->reported_frames = out->frames;
    return true;
}
size_t xz_capture_preroll(const xz_capture_t *c, size_t offset, int16_t *out, size_t capacity) {
    return c ? xz_bargein_preroll_at(c->detector, offset, out, capacity) : 0;
}
void xz_capture_destroy(xz_capture_t *c) {
    if (!c) return;
    xz_bargein_destroy(c->detector);
    volatile unsigned char *p = (volatile unsigned char *)c;
    for (size_t i = 0; i < sizeof *c; ++i) p[i] = 0;
    free(c);
}
