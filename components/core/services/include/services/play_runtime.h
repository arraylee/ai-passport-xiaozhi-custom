#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

// Low two bits: awake/requested/ready/committed. Upper bits identify a request.
// A zero-initialized gate is awake. Cancellation invalidates all old tickets.
typedef struct { atomic_uint state; } play_sleep_gate_t;
bool play_sleep_request(play_sleep_gate_t *gate);
unsigned play_sleep_ticket(const play_sleep_gate_t *gate);
bool play_sleep_publish(play_sleep_gate_t *gate, unsigned ticket);
bool play_sleep_commit(play_sleep_gate_t *gate);
bool play_sleep_cancel(play_sleep_gate_t *gate);
bool play_sleep_blocked(const play_sleep_gate_t *gate);
bool play_sleep_frozen(const play_sleep_gate_t *gate);

// Caller serializes add/take. Completed rounds cannot be displaced by input.
typedef struct { uint32_t total, best, rounds; } play_scores_t;
void play_scores_add(play_scores_t *scores, unsigned score);
play_scores_t play_scores_take(play_scores_t *scores);
