#include "services/answer_session.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    ab_session_t session; ab_audio_meter_t meter;
    ab_session_init(&session); ab_audio_meter_reset(&meter);
    int16_t silence[320] = {0}, voice[320];
    for (unsigned i = 0; i < 320; ++i) voice[i] = i & 1 ? 8000 : -8000;
    assert(ab_session_press(&session, 0, 100));
    assert(!ab_session_press(&session, 1, 200));
    assert(ab_session_release(&session, 1, 2300, ab_audio_meter_snapshot(&meter)) == AB_RELEASE_IGNORED);
    for (unsigned i = 0; i < 120; ++i) ab_audio_meter_push(&meter, silence, 320, 20);
    assert(ab_session_release(&session, 0, 2500, ab_audio_meter_snapshot(&meter)) == AB_RELEASE_TOO_SHORT);
    ab_session_tick(&session, 4100);
    assert(ab_session_view(&session).state == AB_STATE_IDLE);
    assert(ab_session_press(&session, 0, 5000));
    ab_audio_meter_reset(&meter);
    for (unsigned i = 0; i < 20; ++i) ab_audio_meter_push(&meter, voice, 320, 20);
    assert(ab_audio_is_valid(ab_audio_meter_snapshot(&meter)));
    assert(ab_session_release(&session, 0, 6000, ab_audio_meter_snapshot(&meter)) == AB_RELEASE_TOO_SHORT);
    assert(ab_session_press(&session, 0, 6100));
    assert(ab_session_release(&session, 0, 8100, ab_audio_meter_snapshot(&meter)) == AB_RELEASE_REVEAL);
    assert(!ab_session_press(&session, 0, 8101));
    assert(ab_session_set_answer(&session, 23));
    ab_session_tick(&session, 8999); assert(ab_session_view(&session).state == AB_STATE_PAGE_TURN);
    ab_session_tick(&session, 9000); assert(ab_session_view(&session).state == AB_STATE_ANSWER);
    assert(ab_session_view(&session).answer_index == 23);
    assert(ab_session_press(&session, 0, 10000));
    ab_session_audio_error(&session, 10001);
    assert(ab_session_view(&session).state == AB_STATE_MIC_ERROR);
    ab_session_tick(&session, 11601); assert(ab_session_view(&session).state == AB_STATE_IDLE);
    puts("answer session: silence, short hold, voice, owner button, reveal and mic failure PASS");
}
