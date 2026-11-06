#include "services/pet_wish.h"
#include "services/answer_audio_meter.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static uint64_t clock_ms;
static int16_t frame[320];

static void square(int amplitude, int offset)
{
    for (size_t i = 0; i < 320; ++i) {
        frame[i] = (int16_t)(offset + (i & 1 ? amplitude : -amplitude));
    }
}

static void push(pet_wish_t *session, unsigned frames, int amplitude, int offset)
{
    square(amplitude, offset);
    for (unsigned i = 0; i < frames; ++i) {
        clock_ms += 20;
        pet_wish_push(session, frame, 320, 20, clock_ms);
    }
}

static void start(pet_wish_t *session)
{
    pet_wish_init(session);
    clock_ms = 1000;
    assert(pet_wish_start(session, clock_ms));
}

static void listen(pet_wish_t *session, int ambient)
{
    start(session);
    push(session, 25, ambient, 0);
    assert(pet_wish_view(session).phase == PET_WISH_LISTENING);
    assert(pet_wish_view(session).progress == 0);
}

/* A noisy input must never implicitly start a session or skip calibration. */
static void explicit_start_calibration(void)
{
    pet_wish_t session;
    pet_wish_init(&session);
    push(&session, 200, 8000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_IDLE);
    start(&session);
    assert(pet_wish_view(&session).phase == PET_WISH_CALIBRATING);
    assert(!pet_wish_start(&session, clock_ms));
    pet_wish_tick(&session, clock_ms + 500);
    clock_ms += 500;
    assert(pet_wish_view(&session).phase == PET_WISH_CALIBRATING);
    push(&session, 24, 0, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_CALIBRATING);
    push(&session, 1, 0, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_LISTENING);
    assert(pet_wish_view(&session).progress == 0);
}

/* A DC-biased microphone and silence must not open the gift. */
static void silence_dc_timeout(void)
{
    const int offsets[] = {0, 15000, -22000};
    for (unsigned i = 0; i < 3; ++i) {
        pet_wish_t session;
        start(&session);
        push(&session, 750, 0, offsets[i]);
        const pet_wish_view_t view = pet_wish_view(&session);
        assert(view.phase == PET_WISH_TIMED_OUT);
        assert(view.progress == 0 && view.voiced_ms == 0 && view.level == 0);
        assert(view.elapsed_ms == 15000);
    }
}

/* Short, separated loud impulses and claps cannot earn sustained-sound time. */
static void clicks_claps_rejected(void)
{
    pet_wish_t session;
    listen(&session, 0);
    for (unsigned i = 0; i < 30; ++i) {
        memset(frame, 0, sizeof(frame));
        frame[0] = INT16_MAX;
        clock_ms += 20;
        pet_wish_push(&session, frame, 320, 20, clock_ms);
        push(&session, 5, 0, 0);
    }
    for (unsigned i = 0; i < 20; ++i) {
        push(&session, 4, 24000, 0); /* 80 ms clap. */
        push(&session, 5, 0, 0);
    }
    assert(pet_wish_view(&session).phase == PET_WISH_LISTENING);
    assert(pet_wish_view(&session).voiced_ms == 0);
    assert(pet_wish_view(&session).progress == 0);
}

/* Soft syllable-like sound advances by duration, and natural pauses preserve it. */
static void soft_variable_sound_and_pauses(void)
{
    pet_wish_t session;
    listen(&session, 0);
    uint16_t previous = 0;
    for (unsigned syllable = 0; syllable < 10; ++syllable) {
        for (unsigned segment = 0; segment < 10; ++segment) {
            push(&session, 1, 110 + (int)(segment % 4) * 40, 12000);
            const pet_wish_view_t view = pet_wish_view(&session);
            assert(view.progress >= previous && view.progress <= 1000);
            previous = view.progress;
        }
        assert(pet_wish_view(&session).voiced_ms == (syllable + 1) * 200);
        if (syllable != 9) {
            assert(pet_wish_view(&session).phase == PET_WISH_LISTENING);
            const uint16_t before_pause = pet_wish_view(&session).progress;
            push(&session, 10, 0, 0);
            assert(pet_wish_view(&session).progress == before_pause);
        }
    }
    const pet_wish_view_t accepted = pet_wish_view(&session);
    assert(accepted.phase == PET_WISH_ACCEPTED);
    assert(accepted.progress == 1000 && accepted.voiced_ms == 2000);
    pet_wish_cancel(&session);
    pet_wish_audio_error(&session);
    pet_wish_tick(&session, clock_ms + 20000);
    push(&session, 5, 0, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_ACCEPTED);
    assert(pet_wish_view(&session).elapsed_ms == accepted.elapsed_ms);
}

/* Loudness cannot make the duration requirement disappear. */
static void loudness_does_not_accelerate(void)
{
    pet_wish_t session;
    listen(&session, 0);
    push(&session, 50, 30000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_LISTENING);
    assert(pet_wish_view(&session).progress == 500);
    push(&session, 49, 30000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_LISTENING);
    push(&session, 1, 30000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_ACCEPTED);
}

/* A steady fan/room tone heard during calibration cannot subsequently qualify. */
static void constant_ambient_rejected(void)
{
    const int levels[] = {100, 1000, 8000, 30000};
    for (unsigned i = 0; i < 4; ++i) {
        pet_wish_t session;
        listen(&session, levels[i]);
        push(&session, 725, levels[i], 0);
        assert(pet_wish_view(&session).phase == PET_WISH_TIMED_OUT);
        assert(pet_wish_view(&session).progress == 0);
    }
}

/* Detecting relative to ambient still permits a modest increase above it. */
static void sound_above_ambient(void)
{
    pet_wish_t session;
    listen(&session, 1000);
    push(&session, 50, 1800, 0); /* <6 dB above ambient. */
    assert(pet_wish_view(&session).progress == 0);
    push(&session, 100, 2300, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_ACCEPTED);
}

/* Empty captures cannot calibrate, and missing frames cannot join short bursts. */
static void invalid_audio_and_gaps(void)
{
    pet_wish_t session;
    start(&session);
    clock_ms += 700;
    pet_wish_push(&session, NULL, 320, 700, clock_ms);
    assert(pet_wish_view(&session).phase == PET_WISH_CALIBRATING);
    pet_wish_push(&session, frame, 0, 20, ++clock_ms);
    push(&session, 25, 0, 0);
    push(&session, 4, 8000, 0);
    clock_ms += 500;
    pet_wish_tick(&session, clock_ms);
    push(&session, 4, 8000, 0);
    assert(pet_wish_view(&session).progress == 0);
    clock_ms += 20;
    pet_wish_push(&session, NULL, 0, 20, clock_ms);
    push(&session, 4, 8000, 0);
    assert(pet_wish_view(&session).progress == 0);
}

/* A malformed long frame and replayed timestamp cannot manufacture two seconds. */
static void malformed_frame_cannot_finish(void)
{
    pet_wish_t session;
    listen(&session, 0);
    square(30000, 0);
    clock_ms += 20;
    pet_wish_push(&session, frame, 320, UINT32_MAX, clock_ms);
    for (unsigned i = 0; i < 200; ++i) {
        pet_wish_push(&session, frame, 320, 20, clock_ms);
    }
    assert(pet_wish_view(&session).phase == PET_WISH_LISTENING);
    assert(pet_wish_view(&session).progress == 0);
}

/* Retry clears progress, calibration, timeout and terminal audio errors. */
static void timeout_retry_cancel_error(void)
{
    pet_wish_t session;
    listen(&session, 0);
    push(&session, 30, 1000, 0);
    assert(pet_wish_view(&session).progress == 300);
    pet_wish_cancel(&session);
    assert(pet_wish_view(&session).phase == PET_WISH_CANCELLED);
    push(&session, 200, 1000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_CANCELLED);
    assert(pet_wish_start(&session, clock_ms));
    assert(pet_wish_view(&session).progress == 0);
    assert(pet_wish_view(&session).elapsed_ms == 0);
    pet_wish_audio_error(&session);
    assert(pet_wish_view(&session).phase == PET_WISH_MIC_ERROR);
    assert(pet_wish_start(&session, clock_ms));
    pet_wish_tick(&session, clock_ms + 14999);
    assert(pet_wish_view(&session).phase == PET_WISH_CALIBRATING);
    pet_wish_tick(&session, clock_ms + 15000);
    assert(pet_wish_view(&session).phase == PET_WISH_TIMED_OUT);
    clock_ms += 15000;
    assert(pet_wish_start(&session, clock_ms));
    push(&session, 25, 0, 0);
    push(&session, 100, 1000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_ACCEPTED);
}

/* Unsigned clock wrap advances normally; regressions do not underflow elapsed. */
static void clock_wrap_regression_saturation(void)
{
    pet_wish_t session;
    pet_wish_init(&session);
    clock_ms = UINT64_MAX - 299;
    assert(pet_wish_start(&session, clock_ms));
    push(&session, 25, 0, 0);
    assert(pet_wish_view(&session).elapsed_ms == 500);
    pet_wish_tick(&session, clock_ms - 1);
    assert(pet_wish_view(&session).elapsed_ms == 500);
    push(&session, 100, 1000, 0);
    assert(pet_wish_view(&session).phase == PET_WISH_ACCEPTED);
    assert(pet_wish_view(&session).elapsed_ms == 2500);
    assert(pet_wish_start(&session, 0));
    pet_wish_tick(&session, (uint64_t)UINT32_MAX + 2000);
    assert(pet_wish_view(&session).phase == PET_WISH_TIMED_OUT);
    assert(pet_wish_view(&session).elapsed_ms == UINT32_MAX);
}

/* Null inputs are safe and cannot fabricate an accepted result. */
static void null_session(void)
{
    pet_wish_init(NULL);
    assert(!pet_wish_start(NULL, 0));
    pet_wish_push(NULL, NULL, 0, 0, 0);
    pet_wish_tick(NULL, UINT64_MAX);
    pet_wish_cancel(NULL);
    pet_wish_audio_error(NULL);
    assert(pet_wish_view(NULL).phase == PET_WISH_IDLE);
}

typedef void (*test_fn_t)(void);
int main(int argc, char **argv)
{
    const struct { const char *name; test_fn_t run; } tests[] = {
        {"explicit_start_calibration", explicit_start_calibration},
        {"silence_dc_timeout", silence_dc_timeout},
        {"clicks_claps_rejected", clicks_claps_rejected},
        {"soft_variable_sound_and_pauses", soft_variable_sound_and_pauses},
        {"loudness_does_not_accelerate", loudness_does_not_accelerate},
        {"constant_ambient_rejected", constant_ambient_rejected},
        {"sound_above_ambient", sound_above_ambient},
        {"invalid_audio_and_gaps", invalid_audio_and_gaps},
        {"malformed_frame_cannot_finish", malformed_frame_cannot_finish},
        {"timeout_retry_cancel_error", timeout_retry_cancel_error},
        {"clock_wrap_regression_saturation", clock_wrap_regression_saturation},
        {"null_session", null_session},
    };
    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (argc > 1 && strcmp(argv[1], tests[i].name) != 0) continue;
        tests[i].run();
        printf("pet wish: %s PASS\n", tests[i].name);
    }
    return 0;
}
