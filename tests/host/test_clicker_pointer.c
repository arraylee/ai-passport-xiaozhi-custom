#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/clicker_reports.h"

static uint8_t packet[8];
static unsigned attempts, accepted;
static bool fail;
static uint8_t history[16][8];
static unsigned recorded;
static bool send_packet(const uint8_t data[8], void *unused) {
    (void)unused;
    ++attempts;
    if (fail) return false;
    memcpy(packet, data, sizeof(packet));
    if (recorded < 16) memcpy(history[recorded++], data, 8);
    ++accepted; return true;
}
static int dy(const uint8_t *p) { return (int16_t)(p[3] | (p[4] << 8)); }
static void neutral_reads(const clicker_reports_t *state) {
    const uint8_t zero[8] = {0};
    assert(!memcmp(clicker_reports_mouse_current(state), zero, 6));
    assert(!memcmp(clicker_reports_current(state), zero, 8));
    assert(!memcmp(clicker_reports_touch_current(state), zero, 5));
}
int main(void) {
    clicker_reports_t state; clicker_reports_init(&state);
    // A pointer diagnostic must move once on the mouse channel, without a
    // button or wheel event. Repeated ticks/reads must never replay movement.
    assert(!clicker_reports_request(&state, CLICKER_POINTER_TEST, false));
    assert(clicker_reports_request(&state, CLICKER_POINTER_TEST, true));
    assert(clicker_reports_id(&state) == 2);
    clicker_reports_tick(&state, send_packet, NULL);
    const uint8_t probe[8] = {0, 24, 0, 24, 0, 0, 0, 0};
    assert(accepted == 1 && !memcmp(packet, probe, 8));
    assert(!clicker_reports_busy(&state));
    clicker_reports_tick(&state, send_packet, NULL);
    assert(accepted == 1); neutral_reads(&state);
    assert(clicker_reports_request(&state, CLICKER_POINTER_TEST, true));
    clicker_reports_cancel(&state);
    clicker_reports_tick(&state, send_packet, NULL);
    assert(accepted == 1);
    assert(clicker_reports_request(&state, CLICKER_POINTER_TEST, true));
    fail = true; unsigned probe_attempts = attempts;
    clicker_reports_tick(&state, send_packet, NULL);
    clicker_reports_tick(&state, send_packet, NULL);
    assert(attempts == probe_attempts + 1 && state.failed == 1);
    fail = false; clicker_reports_init(&state);
    // A complete gesture drags vertically, releases, then returns with the
    // same two opposite deltas and cadence. Host pointer gain is NOT tested.
    for (unsigned i = 0; i < 100; ++i) {
        clicker_action_t action = i & 1 ? CLICKER_PHONE_PREV : CLICKER_PHONE_NEXT;
        assert(!clicker_reports_request(&state, action, false));
        assert(clicker_reports_request(&state, action, true));
        assert(clicker_reports_id(&state) == 2);
        assert(!clicker_reports_request(&state, CLICKER_NEXT, true));
        unsigned before = accepted;
        recorded = 0;
        for (unsigned tick = 0; clicker_reports_busy(&state); ++tick) {
            assert(tick < 10);
            clicker_reports_tick(&state, send_packet, NULL);
            const uint8_t *read = clicker_reports_mouse_current(&state);
            assert(read[0] == packet[0]);
            for (unsigned b=1; b<6; ++b) assert(read[b] == 0); // reads never replay motion
        }
        assert(accepted == before + 7);
        int sum = 0;
        for (unsigned j=0; j<7; ++j) {
            assert(history[j][0] == (j < 3 ? 1 : 0));
            assert(history[j][1] == 0 && history[j][2] == 0 && history[j][5] == 0);
            assert(history[j][6] == 0 && history[j][7] == 0);
            sum += dy(history[j]);
        }
        int direction = action == CLICKER_PHONE_NEXT ? -1 : 1;
        assert(dy(history[0]) == 0 && dy(history[3]) == 0 && dy(history[6]) == 0);
        assert(dy(history[1]) == direction*32 && dy(history[2]) == direction*48);
        assert(dy(history[4]) == -dy(history[1]) && dy(history[5]) == -dy(history[2]));
        assert(sum == 0);
        assert(state.sent == i + 1 && state.failed == 0);
        neutral_reads(&state); // GATT reads must not repeat the scroll delta.
    }
    // Cancelling any phase leaves no held button and sends no return motion.
    for (unsigned phase=0; phase<7; ++phase) {
        clicker_reports_init(&state);
        assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
        for (unsigned i=0; i<phase; ++i) clicker_reports_tick(&state, send_packet, NULL);
        unsigned start=accepted;
        clicker_reports_cancel(&state);
        for (unsigned i=0; i<3; ++i) clicker_reports_tick(&state, send_packet, NULL);
        assert(accepted == start + (phase >= 1 && phase <= 3 ? 1 : 0));
        assert(!clicker_reports_busy(&state) && state.sent == 0);
        neutral_reads(&state);
    }
    clicker_reports_init(&state);
    unsigned before = accepted;
    assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
    clicker_reports_cancel(&state);
    clicker_reports_tick(&state, send_packet, NULL);
    assert(accepted == before && !clicker_reports_busy(&state));
    assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
    clicker_reports_tick(&state, send_packet, NULL);
    clicker_reports_cancel(&state);
    clicker_reports_tick(&state, send_packet, NULL);
    assert(accepted == before + 2); // Press followed only by release.
    // A rejected press sends no movement; a rejected held movement must only
    // release, never retry its relative delta or attempt a return trajectory.
    fail = true; before = attempts;
    assert(clicker_reports_request(&state, CLICKER_PHONE_PREV, true));
    clicker_reports_tick(&state, send_packet, NULL);
    clicker_reports_tick(&state, send_packet, NULL);
    assert(attempts == before + 1 && state.failed == 1);
    assert(!clicker_reports_busy(&state) && !state.disconnect_required);
    fail = false;
    clicker_reports_init(&state);
    assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
    clicker_reports_tick(&state, send_packet, NULL);
    fail=true; clicker_reports_tick(&state, send_packet, NULL); fail=false;
    clicker_reports_tick(&state, send_packet, NULL);
    assert(!clicker_reports_busy(&state) && state.failed==1 && state.sent==0);
    neutral_reads(&state);
    // A transient release failure must not start the unpressed return until
    // the release is accepted, and must not duplicate the drag movement.
    clicker_reports_init(&state); recorded=0;
    assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
    for (unsigned i=0; i<3; ++i) clicker_reports_tick(&state, send_packet, NULL);
    fail=true; clicker_reports_tick(&state, send_packet, NULL); fail=false;
    assert(recorded==3 && clicker_reports_mouse_current(&state)[0]==1);
    while (clicker_reports_busy(&state)) clicker_reports_tick(&state, send_packet, NULL);
    assert(recorded==7 && state.sent==1 && state.failed==0);
    assert(history[3][0]==0 && dy(history[3])==0 && history[4][0]==0 && dy(history[4])==32);
    // Failed unpressed return ends immediately; no retry/doubled displacement.
    clicker_reports_init(&state);
    assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
    for (unsigned i=0; i<4; ++i) clicker_reports_tick(&state, send_packet, NULL);
    fail=true; before=attempts;
    clicker_reports_tick(&state, send_packet, NULL);
    clicker_reports_tick(&state, send_packet, NULL); fail=false;
    assert(attempts==before+1 && !clicker_reports_busy(&state) && state.failed==1);
    neutral_reads(&state);
    // Failed release retains held state until accepted, or forces disconnect
    // after three tries. This also applies to exit cancellation.
    for (unsigned failures=1; failures<=3; ++failures) {
        clicker_reports_init(&state);
        assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
        clicker_reports_tick(&state, send_packet, NULL);
        clicker_reports_cancel(&state);
        fail=true;
        for (unsigned i=0; i<failures; ++i) clicker_reports_tick(&state, send_packet, NULL);
        assert(clicker_reports_mouse_current(&state)[0] == 1);
        assert(state.disconnect_required == (failures==3));
        fail=false;
        if (failures<3) clicker_reports_tick(&state, send_packet, NULL);
        clicker_reports_disconnected(&state); neutral_reads(&state);
    }
    assert(clicker_reports_request(&state, CLICKER_PHONE_NEXT, true));
    clicker_reports_disconnected(&state);
    before = accepted; clicker_reports_tick(&state, send_packet, NULL);
    assert(accepted == before); neutral_reads(&state);
    assert(clicker_reports_request(&state, CLICKER_NEXT, true));
    assert(clicker_reports_id(&state) == 1);
    puts("iPhone drag: paired return, release before return, no motion replay, cancellation and failed release PASS");
}
