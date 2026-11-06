#pragma once

#include "services/answer_audio_meter.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AB_MIN_HOLD_MS       2000u
#define AB_ERROR_DISPLAY_MS  1600u
#define AB_PAGE_TURN_MS       900u

typedef enum {
    AB_STATE_IDLE,
    AB_STATE_LISTENING,
    AB_STATE_TOO_SHORT,
    AB_STATE_MIC_ERROR,
    AB_STATE_PAGE_TURN,
    AB_STATE_ANSWER,
} ab_state_t;

typedef enum {
    AB_RELEASE_IGNORED,
    AB_RELEASE_TOO_SHORT,
    AB_RELEASE_REVEAL,
} ab_release_result_t;

typedef struct {
    ab_state_t state;
    int8_t owner_button;
    uint64_t started_ms;
    uint64_t deadline_ms;
    size_t answer_index;
    bool answer_ready;
    uint32_t revision;
} ab_session_view_t;

typedef struct {
    ab_session_view_t view;
} ab_session_t;

void ab_session_init(ab_session_t *session);
bool ab_session_press(ab_session_t *session, uint8_t button, uint64_t now_ms);
ab_release_result_t ab_session_release(
    ab_session_t *session,
    uint8_t button,
    uint64_t now_ms,
    ab_audio_summary_t audio);
void ab_session_audio_error(ab_session_t *session, uint64_t now_ms);
void ab_session_tick(ab_session_t *session, uint64_t now_ms);
bool ab_session_set_answer(ab_session_t *session, size_t answer_index);
ab_session_view_t ab_session_view(const ab_session_t *session);
