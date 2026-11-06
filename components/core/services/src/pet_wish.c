#include "services/pet_wish.h"
#include "services/answer_audio_meter.h"

#include <limits.h>

#define WISH_MIN_THRESHOLD_CDBFS (-5200)
#define WISH_AMBIENT_MARGIN_CDBFS 600
#define WISH_MIN_SOUND_RUN_MS 120u
#define WISH_MAX_FRAME_MS 64u
#define WISH_GAP_GRACE_MS 40u

static bool active(const pet_wish_t *session)
{
    return session != NULL && (session->view.phase == PET_WISH_CALIBRATING
        || session->view.phase == PET_WISH_LISTENING);
}

static void finish(pet_wish_t *session, pet_wish_phase_t phase)
{
    session->view.phase = phase;
    session->view.level = 0;
    session->candidate_ms = 0;
}

/* Unsigned subtraction handles clock wrap. Ignore small clock regressions
 * instead of interpreting them as a huge positive time jump. */
static bool advance_clock(pet_wish_t *session, uint64_t now_ms)
{
    if (!active(session)) return false;
    const uint64_t elapsed = now_ms - session->last_clock_ms;
    if (elapsed > UINT64_MAX / 2) return false;
    session->last_clock_ms = now_ms;
    const uint32_t remaining = UINT32_MAX - session->view.elapsed_ms;
    session->view.elapsed_ms += elapsed > remaining ? remaining : (uint32_t)elapsed;
    if (session->view.elapsed_ms >= PET_WISH_TIMEOUT_MS) {
        finish(session, PET_WISH_TIMED_OUT);
        return false;
    }
    return true;
}

void pet_wish_init(pet_wish_t *session)
{
    if (session == NULL) return;
    *session = (pet_wish_t){0};
    session->view.phase = PET_WISH_IDLE;
    session->view.noise_centidbfs = AB_AUDIO_FLOOR_CDBFS;
    session->view.threshold_centidbfs = WISH_MIN_THRESHOLD_CDBFS;
}

bool pet_wish_start(pet_wish_t *session, uint64_t now_ms)
{
    if (session == NULL || active(session)) return false;
    pet_wish_init(session);
    session->view.phase = PET_WISH_CALIBRATING;
    session->last_clock_ms = now_ms;
    session->last_audio_ms = now_ms;
    return true;
}

static void calibrate(pet_wish_t *session, int32_t level, uint32_t frame_ms)
{
    const uint32_t remaining = PET_WISH_CALIBRATION_MS - session->calibration_ms;
    if (frame_ms > remaining) frame_ms = remaining;
    session->calibration_sum += (int64_t)level * frame_ms;
    session->calibration_ms += frame_ms;
    session->view.noise_centidbfs = (int32_t)(session->calibration_sum
        / session->calibration_ms);
    if (session->calibration_ms == PET_WISH_CALIBRATION_MS) {
        const int32_t threshold = session->view.noise_centidbfs
            + WISH_AMBIENT_MARGIN_CDBFS;
        session->view.threshold_centidbfs = threshold > WISH_MIN_THRESHOLD_CDBFS
            ? threshold : WISH_MIN_THRESHOLD_CDBFS;
        session->view.phase = PET_WISH_LISTENING;
    }
}

void pet_wish_push(pet_wish_t *session, const int16_t *samples,
                   size_t sample_count, uint32_t frame_ms, uint64_t now_ms)
{
    if (!advance_clock(session, now_ms)) return;
    const uint64_t since_audio = now_ms - session->last_audio_ms;
    session->last_audio_ms = now_ms;
    if (samples == NULL || sample_count == 0 || frame_ms == 0) {
        session->view.level = 0;
        session->candidate_ms = 0;
        return;
    }
    /* Cap malformed durations and replayed timestamps. A delayed capture can
     * only contribute its own PCM duration, never the missing wall-clock time. */
    if (frame_ms > WISH_MAX_FRAME_MS) frame_ms = WISH_MAX_FRAME_MS;
    if (since_audio > frame_ms + WISH_GAP_GRACE_MS) session->candidate_ms = 0;
    if (frame_ms > since_audio) frame_ms = (uint32_t)since_audio;
    if (frame_ms == 0) return;

    const int32_t level = ab_audio_frame_centidbfs(samples, sample_count);
    session->view.level = ab_audio_level_units(level);
    if (session->view.phase == PET_WISH_CALIBRATING) {
        calibrate(session, level, frame_ms);
        return;
    }
    if (level < session->view.threshold_centidbfs) {
        session->candidate_ms = 0;
        return;
    }

    /* Escrow the beginning of each sound run; only a sustained run earns time.
     * Once qualified, its first 120 ms count too, so soft speech is not punished
     * by a loudness target. Energy alone cannot distinguish speech from music. */
    const bool already_sustained = session->candidate_ms >= WISH_MIN_SOUND_RUN_MS;
    session->candidate_ms += frame_ms;
    if (session->candidate_ms < WISH_MIN_SOUND_RUN_MS) return;
    uint32_t credit = already_sustained ? frame_ms : session->candidate_ms;
    const uint32_t remaining = PET_WISH_REQUIRED_SOUND_MS - session->view.voiced_ms;
    if (credit > remaining) credit = remaining;
    session->view.voiced_ms += credit;
    session->view.progress = (uint16_t)((session->view.voiced_ms * 1000u)
        / PET_WISH_REQUIRED_SOUND_MS);
    if (session->view.voiced_ms == PET_WISH_REQUIRED_SOUND_MS) {
        finish(session, PET_WISH_ACCEPTED);
    }
}

void pet_wish_tick(pet_wish_t *session, uint64_t now_ms)
{
    (void)advance_clock(session, now_ms);
}

void pet_wish_cancel(pet_wish_t *session)
{
    if (active(session)) finish(session, PET_WISH_CANCELLED);
}

void pet_wish_audio_error(pet_wish_t *session)
{
    if (active(session)) finish(session, PET_WISH_MIC_ERROR);
}

pet_wish_view_t pet_wish_view(const pet_wish_t *session)
{
    const pet_wish_view_t empty = {
        .phase = PET_WISH_IDLE,
        .noise_centidbfs = AB_AUDIO_FLOOR_CDBFS,
        .threshold_centidbfs = WISH_MIN_THRESHOLD_CDBFS,
    };
    return session == NULL ? empty : session->view;
}
