#include "services/answer_session.h"

static void bump_revision(ab_session_t *session)
{
    session->view.revision++;
}

void ab_session_init(ab_session_t *session)
{
    if (session == NULL) {
        return;
    }
    session->view.state = AB_STATE_IDLE;
    session->view.owner_button = -1;
    session->view.started_ms = 0;
    session->view.deadline_ms = 0;
    session->view.answer_index = 0;
    session->view.answer_ready = false;
    session->view.revision = 1;
}

static bool can_start(ab_state_t state)
{
    return state == AB_STATE_IDLE
        || state == AB_STATE_TOO_SHORT
        || state == AB_STATE_MIC_ERROR
        || state == AB_STATE_ANSWER;
}

bool ab_session_press(ab_session_t *session, uint8_t button, uint64_t now_ms)
{
    if (session == NULL || button > 2 || !can_start(session->view.state)) {
        return false;
    }
    session->view.state = AB_STATE_LISTENING;
    session->view.owner_button = (int8_t)button;
    session->view.started_ms = now_ms;
    session->view.deadline_ms = 0;
    session->view.answer_ready = false;
    session->view.answer_index = 0;
    bump_revision(session);
    return true;
}

ab_release_result_t ab_session_release(
    ab_session_t *session,
    uint8_t button,
    uint64_t now_ms,
    ab_audio_summary_t audio)
{
    if (session == NULL
        || session->view.state != AB_STATE_LISTENING
        || session->view.owner_button != (int8_t)button) {
        return AB_RELEASE_IGNORED;
    }
    const uint64_t held_ms = now_ms >= session->view.started_ms
        ? now_ms - session->view.started_ms
        : 0;
    session->view.owner_button = -1;
    if (held_ms < AB_MIN_HOLD_MS || !ab_audio_is_valid(audio)) {
        session->view.state = AB_STATE_TOO_SHORT;
        session->view.deadline_ms = now_ms + AB_ERROR_DISPLAY_MS;
        bump_revision(session);
        return AB_RELEASE_TOO_SHORT;
    }
    session->view.state = AB_STATE_PAGE_TURN;
    session->view.deadline_ms = now_ms + AB_PAGE_TURN_MS;
    bump_revision(session);
    return AB_RELEASE_REVEAL;
}

void ab_session_audio_error(ab_session_t *session, uint64_t now_ms)
{
    if (session == NULL || session->view.state != AB_STATE_LISTENING) {
        return;
    }
    session->view.state = AB_STATE_MIC_ERROR;
    session->view.owner_button = -1;
    session->view.deadline_ms = now_ms + AB_ERROR_DISPLAY_MS;
    bump_revision(session);
}

void ab_session_tick(ab_session_t *session, uint64_t now_ms)
{
    if (session == NULL || session->view.deadline_ms == 0
        || now_ms < session->view.deadline_ms) {
        return;
    }
    if (session->view.state == AB_STATE_TOO_SHORT
        || session->view.state == AB_STATE_MIC_ERROR) {
        session->view.state = AB_STATE_IDLE;
        session->view.deadline_ms = 0;
        bump_revision(session);
    } else if (session->view.state == AB_STATE_PAGE_TURN
        && session->view.answer_ready) {
        session->view.state = AB_STATE_ANSWER;
        session->view.deadline_ms = 0;
        bump_revision(session);
    }
}

bool ab_session_set_answer(ab_session_t *session, size_t answer_index)
{
    if (session == NULL || session->view.state != AB_STATE_PAGE_TURN) {
        return false;
    }
    session->view.answer_index = answer_index;
    session->view.answer_ready = true;
    return true;
}

ab_session_view_t ab_session_view(const ab_session_t *session)
{
    const ab_session_view_t empty = {
        .state = AB_STATE_IDLE,
        .owner_button = -1,
        .started_ms = 0,
        .deadline_ms = 0,
        .answer_index = 0,
        .answer_ready = false,
        .revision = 0,
    };
    return session == NULL ? empty : session->view;
}
