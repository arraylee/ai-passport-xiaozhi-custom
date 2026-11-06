#include "services/play_runtime.h"

bool play_sleep_request(play_sleep_gate_t *g) {
    unsigned value = atomic_load(&g->state);
    for (;;) {
        if ((value & 3) != 0) return (value & 3) >= 2;
        if (atomic_compare_exchange_weak(&g->state, &value, value + 1)) return false;
    }
}
unsigned play_sleep_ticket(const play_sleep_gate_t *g) { return atomic_load(&g->state); }
bool play_sleep_publish(play_sleep_gate_t *g, unsigned ticket) {
    return (ticket & 3) == 1 && atomic_compare_exchange_strong(&g->state, &ticket, ticket + 1);
}
bool play_sleep_commit(play_sleep_gate_t *g) {
    unsigned value = atomic_load(&g->state);
    return (value & 3) == 2 && atomic_compare_exchange_strong(&g->state, &value, value + 1);
}
bool play_sleep_cancel(play_sleep_gate_t *g) {
    unsigned value = atomic_load(&g->state);
    for (;;) {
        if ((value & 3) == 3) return false;
        unsigned next = (value & ~3u) + 4;
        if (atomic_compare_exchange_weak(&g->state, &value, next)) return true;
    }
}
bool play_sleep_blocked(const play_sleep_gate_t *g) { return (atomic_load(&g->state) & 3) != 0; }
bool play_sleep_frozen(const play_sleep_gate_t *g) { return (atomic_load(&g->state) & 3) >= 2; }
void play_scores_add(play_scores_t *s, unsigned score) {
    if (score > 999999) score = 999999;
    s->total = score > 99999999u - s->total ? 99999999u : s->total + score;
    if (score > s->best) s->best = score;
    if (s->rounds < UINT32_MAX) ++s->rounds;
}
play_scores_t play_scores_take(play_scores_t *s) {
    play_scores_t value = *s; *s = (play_scores_t){0}; return value;
}
