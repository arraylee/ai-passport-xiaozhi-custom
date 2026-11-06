#include "services/dodge.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static unsigned avoid_and_step(dodge_t *g) {
    int tracked = -1, old_x = 240;
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
        const dodge_obstacle_t *o = &g->obstacles[i];
        if (o->active) { tracked = (int)i; old_x = o->x; }
        if (o->active && o->x < 100 && o->x > -48 && g->lane == o->lane)
            dodge_key(g, g->lane == 0 ? DODGE_DOWN : DODGE_UP);
    }
    dodge_step(g);
    assert(g->mode == DODGE_RUNNING);
    if (tracked < 0)
        for (unsigned i = 0; i < DODGE_OBSTACLES; ++i)
            if (g->obstacles[i].active) tracked = (int)i;
    assert(tracked >= 0);
    return (unsigned)(old_x - g->obstacles[tracked].x);
}

static void test_first_minute_acceleration(void) {
    // Track actual obstacle displacement, including the frame at each speed
    // transition, so changing a displayed speed alone cannot satisfy the test.
    static const struct { unsigned milliseconds, speed, distance; } checkpoints[] = {
        {1960, 4, 196}, {2000, 5, 200}, {4000, 6, 450}, {6000, 7, 750},
        {8000, 8, 1100}, {10000, 9, 1500}, {11960, 9, 1941},
        {12000, 10, 1950}, {60000, 10, 13950}
    };
    dodge_t g; dodge_init(&g, 0, 923); dodge_key(&g, DODGE_OK);
    unsigned distance = 0;
    for (unsigned i = 0; i < sizeof(checkpoints) / sizeof(checkpoints[0]); ++i) {
        while (g.steps * DODGE_STEP_MS < checkpoints[i].milliseconds)
            distance += avoid_and_step(&g);
        assert(g.speed == checkpoints[i].speed);
        assert(distance == checkpoints[i].distance);
    }
    // Pausing at top speed freezes difficulty along with obstacles and score.
    dodge_key(&g, DODGE_OK);
    dodge_t paused = g;
    for (unsigned i = 0; i < 1500; ++i) dodge_step(&g);
    assert(memcmp(&g, &paused, sizeof(g)) == 0);
    dodge_key(&g, DODGE_OK); // Continue at the same speed.
    avoid_and_step(&g);
    assert(g.speed == 10);
    dodge_key(&g, DODGE_OK); dodge_key(&g, DODGE_DOWN); dodge_key(&g, DODGE_OK);
    assert(g.speed == 4 && g.steps == 0 && g.score == 0);
    for (unsigned i = 0; i < DODGE_SPEEDUP_MS / DODGE_STEP_MS - 1; ++i) avoid_and_step(&g);
    assert(g.speed == 4);
    dodge_key(&g, DODGE_OK); paused = g;
    for (unsigned i = 0; i < 1500; ++i) dodge_step(&g);
    assert(memcmp(&g, &paused, sizeof(g)) == 0);
    dodge_key(&g, DODGE_OK);
    assert(avoid_and_step(&g) == 2 && g.speed == 5);
    unsigned first = avoid_and_step(&g);
    assert(first == 2 && avoid_and_step(&g) == 3); // 125 px/s over two 20 ms ticks.

    // Active-time counter rollover must never reset the top speed.
    g.steps = UINT_MAX - 1; g.speed = 10;
    assert(avoid_and_step(&g) == 5 && g.steps == UINT_MAX);
    assert(avoid_and_step(&g) == 5 && g.steps == 0 && g.speed == 10);
    assert(avoid_and_step(&g) == 5 && g.steps == 1 && g.speed == 10);
}

static void test_top_speed_collision_edges(void) {
    static const struct { int x; bool collision; } cases[] = {
        {57, false}, {56, true}, {-2, true}, {-3, false}
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        dodge_t g; dodge_init(&g, 8, 923); dodge_key(&g, DODGE_OK);
        g.speed = 10; g.score = 12;
        g.obstacles[0] = (dodge_obstacle_t){.active = true, .lane = 1, .x = cases[i].x};
        dodge_step(&g);
        // At 5 px per 20 ms frame, exact edge contact is safe, a 1 px overlap is a hit.
        assert((g.mode == DODGE_OVER) == cases[i].collision);
        if (cases[i].collision) {
            assert(g.best == 12);
            dodge_key(&g, DODGE_OK);
            assert(g.mode == DODGE_RUNNING && g.best == 12 && g.score == 0 && g.speed == 4);
        }
    }
    dodge_t g; dodge_init(&g, 0, 923); dodge_key(&g, DODGE_OK);
    for (unsigned i = 0; i < 4; ++i) dodge_key(&g, DODGE_UP);
    assert(g.lane == 0);
    for (unsigned i = 0; i < 4; ++i) dodge_key(&g, DODGE_DOWN);
    assert(g.lane == 2);
    g.speed = 10;
    g.obstacles[0] = (dodge_obstacle_t){.active = true, .lane = 1, .x = 41};
    dodge_step(&g);
    assert(g.mode == DODGE_RUNNING && g.lane == 2);
}

static void test_fast_run_reachability(void) {
    // A player that takes 200 ms to react and only moves one lane at a time
    // must still be able to survive varied obstacle sequences at the speed cap.
    for (uint32_t seed = 0; seed < 64; ++seed) {
        dodge_t g; dodge_init(&g, 0, seed); dodge_key(&g, DODGE_OK);
        unsigned reaction = 0, spawns = 0, last_spawn = 0;
        for (unsigned t = 0; t < 400000 / DODGE_STEP_MS; ++t) {
            if (reaction && --reaction == 0)
                dodge_key(&g, g.lane == 0 ? DODGE_DOWN : DODGE_UP);
            if (!reaction) {
                for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
                    const dodge_obstacle_t *o = &g.obstacles[i];
                    if (o->active && o->x <= 120 && o->x + 40 > DODGE_PLAYER_X + 6 &&
                        g.lane == o->lane) reaction = 200 / DODGE_STEP_MS;
                }
            }
            bool was_active[DODGE_OBSTACLES];
            for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) was_active[i] = g.obstacles[i].active;
            dodge_step(&g); assert(g.mode == DODGE_RUNNING);
            unsigned overlapping = 0;
            for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
                const dodge_obstacle_t *o = &g.obstacles[i];
                if (!o->active) continue;
                if (!was_active[i]) {
                    // Even at the cap a fresh column is at least 600 ms later.
                    if (spawns) assert((t - last_spawn) * DODGE_STEP_MS >= 600);
                    last_spawn = t; ++spawns;
                }
                if (o->x + 8 < DODGE_PLAYER_X + 34 && o->x + 40 > DODGE_PLAYER_X + 6)
                    ++overlapping;
                for (unsigned j = i + 1; j < DODGE_OBSTACLES; ++j) {
                    if (!g.obstacles[j].active) continue;
                    int gap = o->x - g.obstacles[j].x;
                    assert(gap >= DODGE_SPAWN_GAP || gap <= -DODGE_SPAWN_GAP);
                }
            }
            assert(overlapping <= 1);
        }
        assert(g.speed == 10 && g.score > 600 && spawns > 600);
    }
}

int main(void) {
    test_first_minute_acceleration();
    test_fast_run_reachability();
    test_top_speed_collision_edges();
    dodge_t g; dodge_init(&g, 8, 42);
    dodge_key(&g, DODGE_DOWN); dodge_key(&g, DODGE_OK); assert(g.mode == DODGE_EXIT);
    dodge_init(&g, 8, 42); dodge_key(&g, DODGE_OK);
    dodge_step(&g);
    assert(g.mode == DODGE_RUNNING);
    unsigned live = 0;
    for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) live += g.obstacles[i].active;
    assert(live == 1);
    dodge_key(&g, DODGE_OK); assert(g.mode == DODGE_PAUSED);
    dodge_t paused = g;
    for (int i = 0; i < 500; ++i) dodge_step(&g);
    assert(memcmp(&g, &paused, sizeof(g)) == 0); // No motion, score, RNG or time advance.
    dodge_key(&g, DODGE_DOWN); dodge_key(&g, DODGE_OK); // Restart from pause.
    assert(g.mode == DODGE_RUNNING && g.score == 0 && g.best == 8 && g.steps == 0);
    dodge_key(&g, DODGE_OK); dodge_key(&g, DODGE_DOWN); dodge_key(&g, DODGE_DOWN);
    dodge_key(&g, DODGE_OK); assert(g.mode == DODGE_EXIT);
    dodge_init(&g, 0, 42); dodge_key(&g, DODGE_OK);
    // A real collision terminates the run and the result page also has short back.
    g.score = 5; g.obstacles[0] = (dodge_obstacle_t){.active=true, .lane=1, .x=35};
    dodge_step(&g); assert(g.mode == DODGE_OVER && g.best == 5);
    dodge_key(&g, DODGE_DOWN); dodge_key(&g, DODGE_OK); assert(g.mode == DODGE_EXIT);
    // Thousands of live spawns: obstacle columns cannot cover the player together.
    dodge_init(&g, 0, 923); dodge_key(&g, DODGE_OK);
    for (int t = 0; t < 10000; ++t) {
        for (unsigned i = 0; i < DODGE_OBSTACLES; ++i) {
            dodge_obstacle_t *o = &g.obstacles[i];
            if (o->active && o->x < 90 && o->x > -48 && g.lane == o->lane)
                dodge_key(&g, g.lane == 0 ? DODGE_DOWN : DODGE_UP);
            for (unsigned j = i + 1; j < DODGE_OBSTACLES; ++j) {
                if (!o->active || !g.obstacles[j].active) continue;
                int gap = o->x - g.obstacles[j].x;
                assert(gap >= DODGE_SPAWN_GAP || gap <= -DODGE_SPAWN_GAP);
            }
        }
        dodge_step(&g); assert(g.mode == DODGE_RUNNING);
        assert(g.speed <= 10);
    }
    assert(g.score > 100);
    puts("dodge: 12-second acceleration and displacement, pause/restart, rollover, collision, 64-seed delayed-input fairness passed");
}
