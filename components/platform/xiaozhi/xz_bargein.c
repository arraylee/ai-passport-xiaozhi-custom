#include "xz_bargein.h"
#include <stdlib.h>
#include "webrtc/common_audio/vad/vad_core.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#define DETECT_FRAMES 16u
#define CORRELATION_TAIL 128u
#define CORRELATION_WINDOW 64u
#define CORRELATION_HISTORY (CORRELATION_TAIL + CORRELATION_WINDOW)
struct xz_bargein {
    xz_echo_t *echo;
    VadInstT vad;
    int16_t vad_frame[80];
    int16_t far_history[CORRELATION_HISTORY];
    int16_t near_history[CORRELATION_WINDOW];
    unsigned far_at, near_at;
    unsigned vad_at;
    bool vad_speech;
    int16_t *reference;
    int16_t preroll[XZ_BARGEIN_PREROLL];
    unsigned reference_at, preroll_at, preroll_count, delay;
    unsigned frames, aligned, speech, far_hangover;
    uint32_t noise_energy;
    uint64_t microphone_energy_sum, reference_energy_sum, residual_energy_sum;
    uint32_t clipped_frames;
    uint32_t candidate_frames, echo_veto_frames, last_clipped_frame, trigger_far_hangover;
    bool triggered;
};

static uint32_t energy(const int16_t *samples, bool *clipped) {
    int64_t sum = 0;
    uint64_t squares = 0;
    for (unsigned i = 0; i < XZ_ECHO_FRAME; ++i) {
        int32_t x = samples[i];
        sum += x; squares += (uint64_t)((int64_t)x * x);
        if (x >= 32000 || x <= -32000) *clipped = true;
    }
    return (uint32_t)((squares - (uint64_t)(sum * sum) / XZ_ECHO_FRAME) / XZ_ECHO_FRAME);
}

/* A nonlinear loudspeaker can leave speech-shaped residuals even when the
 * linear canceller is healthy. Compare64ms windows at1kHz across a128ms acoustic tail.
 * This also covers room reflections beyond the32ms adaptive filter. The longer window distinguishes independent voiced speech
 * from one coincident period. No allocation or audio logging in this path. */
static bool coherent_echo(xz_bargein_t *b, const int16_t *ref, const int16_t *mic, bool evaluate) {
    for (unsigned i = 0; i < XZ_ECHO_FRAME; i += 8) {
        int32_t far_sum = 0, near_sum = 0;
        for (unsigned j = 0; j < 8; ++j) { far_sum += ref[i+j]; near_sum += mic[i+j]; }
        b->far_history[b->far_at] = far_sum / 128;
        b->near_history[b->near_at] = near_sum / 128;
        if (++b->far_at == CORRELATION_HISTORY) b->far_at = 0;
        if (++b->near_at == CORRELATION_WINDOW) b->near_at = 0;
    }
    if (!evaluate) return false;
    int32_t sum = 0, squares = 0;
    for (unsigned i = 0; i < CORRELATION_WINDOW; ++i) {
        int32_t x = b->near_history[i]; sum += x; squares += x * x;
    }
    int32_t near_energy = squares - (int32_t)((int64_t)sum * sum / CORRELATION_WINDOW);
    if (near_energy < 1600) return false;
    for (unsigned lag = 0; lag <= CORRELATION_TAIL; ++lag) {
        unsigned at = (b->far_at + CORRELATION_HISTORY - CORRELATION_WINDOW - lag) % CORRELATION_HISTORY;
        unsigned near_at = b->near_at;
        int32_t far_sum = 0, far_squares = 0, cross = 0;
        for (unsigned i = 0; i < CORRELATION_WINDOW; ++i) {
            int32_t x = b->far_history[at];
            far_sum += x; far_squares += x * x; cross += x * b->near_history[near_at];
            if (++at == CORRELATION_HISTORY) at = 0;
            if (++near_at == CORRELATION_WINDOW) near_at = 0;
        }
        int32_t far_energy = far_squares - (int32_t)((int64_t)far_sum * far_sum / CORRELATION_WINDOW);
        if (far_energy < 1600) continue;
        cross -= (int32_t)((int64_t)sum * far_sum / CORRELATION_WINDOW);
        // Samples scaled by16 bound all products below2^58. Correlation^2
        // above9/16 means most microphone energy still follows the speaker.
        if ((uint64_t)((int64_t)cross * cross) >
            ((uint64_t)near_energy * (uint32_t)far_energy / 16u) * 9u) return true;
    }
    return false;
}

xz_bargein_t *xz_bargein_create(unsigned delay) {
    if (delay > XZ_BARGEIN_DELAY_MAX) return NULL;
    /* Reserve the largest contiguous block before the history rings. */
    xz_echo_t *echo = xz_echo_create();
    if (!echo) return NULL;
    /* This CPU-only history/VAD state never reaches DMA. On C3, keep it in
     * RTC fast RAM so I2S/Wi-Fi retain scarce DMA-capable SRAM. Default malloc
     * prefers SRAM and can strand free RTC memory while a512B DMA request fails.
     * destroy() wipes this retained-memory allocation before returning it. */
#ifdef ESP_PLATFORM
    xz_bargein_t *b = heap_caps_calloc(1, sizeof *b, MALLOC_CAP_RTCRAM | MALLOC_CAP_8BIT);
#else
    xz_bargein_t *b = calloc(1, sizeof *b);
#endif
    if (!b) { xz_echo_destroy(echo); return NULL; }
    if (delay) {
        b->reference = calloc(delay + 1u, sizeof *b->reference);
        if (!b->reference) { xz_echo_destroy(echo); free(b); return NULL; }
    }
    b->echo = echo; b->delay = delay; b->noise_energy = 6400;
    if (WebRtcVad_InitCore(&b->vad) || WebRtcVad_set_mode_core(&b->vad, 3)) {
        xz_bargein_destroy(b); return NULL;
    }
    return b;
}

bool xz_bargein_feed(xz_bargein_t *b, const int16_t *reference, const int16_t *mic) {
    if (!b || !reference || !mic) return false;
    int16_t delayed[XZ_ECHO_FRAME], clean[XZ_ECHO_FRAME];
    for (unsigned i = 0; i < XZ_ECHO_FRAME; ++i) {
        delayed[i] = reference[i];
        if (b->delay) {
            b->reference[b->reference_at] = reference[i];
            delayed[i] = b->reference[(b->reference_at + 1u) % (b->delay + 1u)];
            b->reference_at = (b->reference_at + 1u) % (b->delay + 1u);
        }
    }
    if (!xz_echo_process(b->echo, mic, delayed, clean)) return false;
    if (b->triggered) return true; /* Preserve the exact onset pre-roll. */
    for (unsigned i = 0; i < XZ_ECHO_FRAME; ++i) {
        b->vad_frame[b->vad_at++] = clean[i];
        if (b->vad_at == 80) {
            b->vad_speech = WebRtcVad_CalcVad8khz(&b->vad, b->vad_frame, 80) > 0;
            b->vad_at = 0;
        }
        b->preroll[b->preroll_at] = clean[i];
        b->preroll_at = (b->preroll_at + 1u) % XZ_BARGEIN_PREROLL;
        if (b->preroll_count < XZ_BARGEIN_PREROLL) ++b->preroll_count;
    }
    bool clipped = false, ignored = false;
    uint32_t raw_energy = energy(mic, &clipped);
    uint32_t reference_energy = energy(delayed, &clipped);
    uint32_t clean_energy = energy(clean, &ignored);
    if (b->frames < UINT32_MAX) {
        ++b->frames;
        b->microphone_energy_sum += raw_energy;
        b->reference_energy_sum += reference_energy;
        b->residual_energy_sum += clean_energy;
        b->clipped_frames += clipped;
        if (clipped) b->last_clipped_frame = b->frames;
    }
    bool far_active = reference_energy >= 6400;
    if (far_active) b->far_hangover = 8;
    else if (b->far_hangover) --b->far_hangover;
    if (!clipped && far_active && raw_energy > 6400 &&
        (uint64_t)clean_energy * 4u < raw_energy && b->aligned < 64) ++b->aligned;

    /* A speaker at zero gain has a digital reference but no acoustic echo;
     * requiring prior measured cancellation would lock out real speech there.
     * Startup settling plus independent residual and sustained VAD below gate
     * onset without requiring the user to wait for an echo-training phase. */
    bool reliable = b->frames >= 16;
    uint64_t threshold = (uint64_t)b->noise_energy * 6u;
    if (threshold < 32400) threshold = 32400; /* RMS180 ADC counts. */
    /* Residual far-end speech is still speech to a VAD. During playback require
     * a substantial independent component, not merely energy above silence. */
    bool independent = !b->far_hangover || (uint64_t)clean_energy * 5u > (uint64_t)raw_energy * 2u;
    bool candidate = reliable && independent && b->vad_speech && !clipped && clean_energy > threshold;
    bool follows_reference = coherent_echo(b, delayed, mic, candidate);
    if (candidate && b->candidate_frames < UINT32_MAX) ++b->candidate_frames;
    if (follows_reference && b->echo_veto_frames < UINT32_MAX) ++b->echo_veto_frames;
    bool speech = candidate && !follows_reference;
    if (speech) {
        if (++b->speech >= DETECT_FRAMES) {
            b->triggered = true; b->trigger_far_hangover = b->far_hangover;
        }
    } else {
        if (b->speech) --b->speech;
        /* Do not learn the onset of an utterance into the noise estimate. */
        if (!clipped && clean_energy < (uint64_t)b->noise_energy * 3u) {
            uint32_t estimate = (uint32_t)(((uint64_t)b->noise_energy * 31u + clean_energy) / 32u);
            b->noise_energy = estimate < 1600 ? 1600 : estimate > 160000 ? 160000 : estimate;
        }
    }
    if (clipped) b->speech = 0; /* Saturation cannot establish near-end speech. */
    return true;
}

bool xz_bargein_triggered(const xz_bargein_t *b) { return b && b->triggered; }
bool xz_bargein_metrics(const xz_bargein_t *b, xz_bargein_metrics_t *out) {
    if (!b || !out) return false;
    *out = (xz_bargein_metrics_t){
        .frames = b->frames, .clipped_frames = b->clipped_frames,
        .aligned_frames = b->aligned,
        .candidate_frames = b->candidate_frames, .echo_veto_frames = b->echo_veto_frames,
        .last_clipped_frame = b->last_clipped_frame, .trigger_far_hangover = b->trigger_far_hangover,
        .microphone_energy_sum = b->microphone_energy_sum,
        .reference_energy_sum = b->reference_energy_sum,
        .residual_energy_sum = b->residual_energy_sum,
    };
    return true;
}
size_t xz_bargein_preroll(const xz_bargein_t *b, int16_t *out, size_t capacity) {
    if (!b || !out || !capacity) return 0;
    size_t count = b->preroll_count < capacity ? b->preroll_count : capacity;
    unsigned start = (b->preroll_at + XZ_BARGEIN_PREROLL - (unsigned)count) % XZ_BARGEIN_PREROLL;
    for (size_t i = 0; i < count; ++i) out[i] = b->preroll[(start + i) % XZ_BARGEIN_PREROLL];
    return count;
}
size_t xz_bargein_preroll_at(const xz_bargein_t *b, size_t offset, int16_t *out, size_t capacity) {
    if (!b || !out || offset >= b->preroll_count) return 0;
    size_t count = b->preroll_count - offset;
    if (count > capacity) count = capacity;
    unsigned start = (b->preroll_at + XZ_BARGEIN_PREROLL - b->preroll_count + (unsigned)offset) % XZ_BARGEIN_PREROLL;
    for (size_t i = 0; i < count; ++i) out[i] = b->preroll[(start + i) % XZ_BARGEIN_PREROLL];
    return count;
}
static bool clear_detection_history(xz_bargein_t *b) {
    if (b->reference) memset(b->reference, 0, (b->delay + 1u) * sizeof *b->reference);
    memset(b->preroll, 0, sizeof b->preroll);
    memset(b->vad_frame, 0, sizeof b->vad_frame);
    memset(b->far_history, 0, sizeof b->far_history);
    memset(b->near_history, 0, sizeof b->near_history);
    b->far_at = b->near_at = 0;
    b->reference_at = b->preroll_at = b->preroll_count = b->frames = 0;
    b->microphone_energy_sum = b->reference_energy_sum = b->residual_energy_sum = 0;
    b->clipped_frames = b->aligned = 0;
    b->candidate_frames = b->echo_veto_frames = b->last_clipped_frame = b->trigger_far_hangover = 0;
    b->speech = b->far_hangover = b->vad_at = 0;
    b->triggered = b->vad_speech = false;
    b->noise_energy = 6400;
    return !WebRtcVad_InitCore(&b->vad) && !WebRtcVad_set_mode_core(&b->vad, 3);
}
bool xz_bargein_resume(xz_bargein_t *b) {
    if (!b) return false;
    /* Flush the previous turn's sample history while retaining learned acoustic
     * weights. No allocation and no private samples survive into a new turn. */
    int16_t zero[XZ_ECHO_FRAME] = {0}, clean[XZ_ECHO_FRAME];
    for (unsigned i = 0; i < 16; ++i)
        if (!xz_echo_process(b->echo, zero, zero, clean)) return false;
    return clear_detection_history(b);
}
bool xz_bargein_discontinuity(xz_bargein_t *b) {
    // Missing samples invalidate the adaptive filter's time history. Reset it
    // cheaply; running many synthetic frames here would itself overrun DMA.
    return b && xz_echo_reset(b->echo) && clear_detection_history(b);
}
void xz_bargein_destroy(xz_bargein_t *b) {
    if (!b) return;
    xz_echo_destroy(b->echo);
    if (b->reference) {
        volatile int16_t *r = b->reference;
        for (unsigned i = 0; i <= b->delay; ++i) r[i] = 0;
        free(b->reference);
    }
    volatile unsigned char *bytes = (volatile unsigned char *)b;
    for (size_t i = 0; i < sizeof *b; ++i) bytes[i] = 0;
    free(b);
}
