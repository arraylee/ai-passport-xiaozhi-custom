#include "services/play_runtime.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    play_sleep_gate_t gate = {0};
    assert(!play_sleep_request(&gate));
    unsigned old = play_sleep_ticket(&gate);
    assert((old & 3) == 1 && play_sleep_blocked(&gate));
    // Simulate a delayed Flash completion across cancel + a new request.
    assert(play_sleep_cancel(&gate));
    assert(!play_sleep_request(&gate));
    unsigned current = play_sleep_ticket(&gate);
    assert(current != old && !play_sleep_publish(&gate, old));
    assert(!play_sleep_frozen(&gate) && !play_sleep_request(&gate));
    assert(play_sleep_publish(&gate, current));
    assert(play_sleep_frozen(&gate) && play_sleep_request(&gate));
    assert(play_sleep_cancel(&gate)); // User input can still cancel READY.
    assert(!play_sleep_commit(&gate) && !play_sleep_blocked(&gate));
    assert(!play_sleep_request(&gate));
    assert(play_sleep_publish(&gate, play_sleep_ticket(&gate)));
    assert(play_sleep_commit(&gate));
    assert(!play_sleep_cancel(&gate)); // Once peripherals shut down, no new work.
    assert(play_sleep_frozen(&gate));
    play_scores_t scores = {0};
    // Three rounds finish while the input queue/worker is blocked.
    play_scores_add(&scores, 7); play_scores_add(&scores, 30); play_scores_add(&scores, 2);
    play_scores_t batch = play_scores_take(&scores);
    assert(batch.total == 39 && batch.best == 30 && batch.rounds == 3);
    assert(play_scores_take(&scores).rounds == 0); // Drain does not reapply old rounds.
    play_scores_add(&scores, 0); assert(play_scores_take(&scores).rounds == 1);
    for (int i = 0; i < 200; ++i) play_scores_add(&scores, 2000000);
    assert(scores.total == 99999999 && scores.best == 999999);
    puts("play runtime: stale sleep completion, freeze/commit/cancel and score accumulation passed");
}
