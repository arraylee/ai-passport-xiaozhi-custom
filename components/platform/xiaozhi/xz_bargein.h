#pragma once
#include "xz_echo.h"
typedef struct xz_bargein xz_bargein_t;
/* Numeric observations only: no samples, speech text or extra allocation. */
typedef struct {
    uint64_t microphone_energy_sum, reference_energy_sum, residual_energy_sum;
    uint32_t frames, clipped_frames, aligned_frames;
    uint32_t candidate_frames, echo_veto_frames, last_clipped_frame, trigger_far_hangover;
} xz_bargein_metrics_t;
enum { XZ_BARGEIN_PREROLL = 2400, XZ_BARGEIN_DELAY_MAX = 768 };
/* Both streams are8kHz,64samples, on the same capture clock. delay_samples
 * accounts for output DMA latency before the32ms adaptive filter tail. */
xz_bargein_t *xz_bargein_create(unsigned delay_samples);
bool xz_bargein_feed(xz_bargein_t *,const int16_t *reference,const int16_t *mic);
bool xz_bargein_triggered(const xz_bargein_t *);
size_t xz_bargein_preroll(const xz_bargein_t *,int16_t *out,size_t capacity);
void xz_bargein_destroy(xz_bargein_t *);

bool xz_bargein_resume(xz_bargein_t *);
bool xz_bargein_discontinuity(xz_bargein_t *);
size_t xz_bargein_preroll_at(const xz_bargein_t *, size_t offset, int16_t *out, size_t capacity);
bool xz_bargein_metrics(const xz_bargein_t *, xz_bargein_metrics_t *);
