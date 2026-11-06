#include "services/dodge.h"
#include <string.h>

void dodge_init(dodge_t *g, unsigned best, uint32_t seed) {
    memset(g, 0, sizeof(*g));
    g->mode = DODGE_READY; g->best = best; g->rng = seed; g->lane = 1;
    g->speed = DODGE_START_SPEED;
}
static void start(dodge_t *g) {
    memset(g->obstacles, 0, sizeof(g->obstacles));
    g->mode = DODGE_RUNNING; g->score = 0; g->steps = 0; g->lane = 1;
    g->move_remainder = 0;
    g->speed = DODGE_START_SPEED;
}
void dodge_key(dodge_t *g, dodge_key_t key) {
    if (g->mode == DODGE_EXIT) return;
    if (key == DODGE_BACK) { g->mode = DODGE_EXIT; return; }
    if (g->mode == DODGE_RUNNING) {
        if (key == DODGE_UP && g->lane > 0) --g->lane;
        else if (key == DODGE_DOWN && g->lane < 2) ++g->lane;
        else if (key == DODGE_OK) { g->mode = DODGE_PAUSED; g->selection = 0; }
        return;
    }
    unsigned count = g->mode == DODGE_PAUSED ? 3 : 2;
    if (key == DODGE_UP) g->selection = (g->selection + count - 1) % count;
    else if (key == DODGE_DOWN) g->selection = (g->selection + 1) % count;
    else if (key == DODGE_OK) {
        if (g->selection == count - 1) g->mode = DODGE_EXIT;
        else if (g->mode == DODGE_PAUSED && g->selection == 0) g->mode = DODGE_RUNNING;
        else start(g);
    }
}
static unsigned random_value(dodge_t *g, unsigned bound) {
    g->rng = g->rng * 1664525u + 1013904223u;
    return (g->rng >> 16) % bound;
}
void dodge_step(dodge_t *g) {
    if (g->mode != DODGE_RUNNING) return;
    int rightmost = -1000, free_slot = -1;
    // The game tick is now 20ms instead of 40ms. Keep the old pixels/second
    // difficulty by distributing each whole-pixel movement across two ticks.
    unsigned move_pixels = g->move_remainder + g->speed;
    g->move_remainder = (uint8_t)(move_pixels & 1u);
    move_pixels >>= 1;
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
        dodge_obstacle_t *o = &g->obstacles[i];
        if (o->active) { if (o->x > rightmost) rightmost = o->x; }
        else free_slot = (int)i;
    }
    if (free_slot >= 0 && rightmost <= 240 - DODGE_SPAWN_GAP) {
        g->obstacles[free_slot] = (dodge_obstacle_t){.active = true, .x = 240,
            .lane = random_value(g, 3), .art = random_value(g, 4)};
    }
    ++g->steps;
    unsigned next_speed = g->speed;
    // Only active play advances this clock: start at 100 px/s and add 25 px/s
    // every two seconds, reaching the safe 250 px/s cap after 12 seconds.
    if (next_speed < DODGE_MAX_SPEED && g->steps % (DODGE_SPEEDUP_MS / DODGE_STEP_MS) == 0)
        ++next_speed;
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
        dodge_obstacle_t *o = &g->obstacles[i];
        if (!o->active) continue;
        o->x -= (int16_t)move_pixels;
        // Hit boxes sit inside the artwork: transparent margins never count.
        if (o->lane == g->lane && o->x + 8 < DODGE_PLAYER_X + 34 &&
            o->x + 40 > DODGE_PLAYER_X + 6) {
            g->mode = DODGE_OVER; g->selection = 0;
            if (g->score > g->best) g->best = g->score;
            return;
        }
        if (!o->passed && o->x + DODGE_SPRITE_W < DODGE_PLAYER_X) {
            o->passed = true; ++g->score;
        }
        if (o->x < -DODGE_SPRITE_W) o->active = false;
    }
    // All obstacles move at the same speed in a tick, preserving the safe gap.
    g->speed = next_speed;
}
