#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xz_bargein.h"
typedef struct xz_capture xz_capture_t;
xz_capture_t *xz_capture_create(void);
bool xz_capture_begin(xz_capture_t *, unsigned rate);
/* ES8311 ADC left, DACR right; both arrive in one I2S clocked frame. */
bool xz_capture_feed(xz_capture_t *, const int16_t *stereo, size_t frames);
bool xz_capture_triggered(const xz_capture_t *);
bool xz_capture_discontinuity(xz_capture_t *);
unsigned xz_capture_gap_count(const xz_capture_t *);
size_t xz_capture_preroll(const xz_capture_t *, size_t offset, int16_t *out, size_t capacity);
void xz_capture_destroy(xz_capture_t *);
/* Startup, trigger, then once per second of processed audio; resets per reply. */
bool xz_capture_observe(xz_capture_t *, xz_bargein_metrics_t *);
