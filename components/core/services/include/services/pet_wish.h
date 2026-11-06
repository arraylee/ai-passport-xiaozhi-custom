#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PET_WISH_CALIBRATION_MS 500u
#define PET_WISH_REQUIRED_SOUND_MS 2000u
#define PET_WISH_TIMEOUT_MS 15000u

typedef enum {
    PET_WISH_IDLE,
    PET_WISH_CALIBRATING,
    PET_WISH_LISTENING,
    PET_WISH_ACCEPTED,
    PET_WISH_TIMED_OUT,
    PET_WISH_CANCELLED,
    PET_WISH_MIC_ERROR,
} pet_wish_phase_t;

typedef struct {
    pet_wish_phase_t phase;
    uint16_t progress; /* 0..1000; nondecreasing until the next start. */
    uint16_t level;    /* Current sound energy, 0..1000. */
    uint32_t elapsed_ms;
    uint32_t voiced_ms; /* Sustained above-threshold SOUND, not recognized speech. */
    int32_t noise_centidbfs;
    int32_t threshold_centidbfs;
} pet_wish_view_t;

/* Caller-owned storage. Serialize all access; no allocation or platform calls. */
typedef struct {
    pet_wish_view_t view;
    uint64_t last_clock_ms;
    uint64_t last_audio_ms;
    int64_t calibration_sum;
    uint32_t calibration_ms;
    uint32_t candidate_ms;
} pet_wish_t;

void pet_wish_init(pet_wish_t *session);
/* Explicit start or retry; an active session cannot be restarted accidentally. */
bool pet_wish_start(pet_wish_t *session, uint64_t now_ms);
/* PCM frames should be <=64 ms. Oversized duration is capped, never backfilled.
 * now_ms is the frame-end monotonic clock, using the same clock as tick/start.
 * Invalid/empty frames cannot contribute sound or calibration. */
void pet_wish_push(pet_wish_t *session, const int16_t *samples,
                   size_t sample_count, uint32_t frame_ms, uint64_t now_ms);
/* Tick even when audio stalls so the 15-second wall timeout still takes effect. */
void pet_wish_tick(pet_wish_t *session, uint64_t now_ms);
void pet_wish_cancel(pet_wish_t *session);
void pet_wish_audio_error(pet_wish_t *session);
pet_wish_view_t pet_wish_view(const pet_wish_t *session);

/* ACCEPTED is terminal and only authorizes the app's next step. The app must
 * commit acquisition successfully BEFORE revealing a pet; this service never
 * chooses a pet, saves data, owns a microphone, or recognizes words. */
