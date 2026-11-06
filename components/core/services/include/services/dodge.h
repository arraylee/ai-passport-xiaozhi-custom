#pragma once
#include <stdbool.h>
#include <stdint.h>

enum { DODGE_LANES = 3, DODGE_OBSTACLES = 3, DODGE_STEP_MS = 20,
       DODGE_START_SPEED = 4, DODGE_MAX_SPEED = 10, DODGE_SPEEDUP_MS = 2000,
       // At 250 px/s: 600 ms between columns, with 360 ms clear of the
       // 60 px collision window for a 200 ms reaction plus input polling.
       DODGE_SPAWN_GAP = 150, DODGE_PLAYER_X = 26, DODGE_SPRITE_W = 48 };
typedef enum { DODGE_READY, DODGE_RUNNING, DODGE_PAUSED, DODGE_OVER, DODGE_EXIT } dodge_mode_t;
typedef enum { DODGE_UP, DODGE_DOWN, DODGE_OK, DODGE_BACK } dodge_key_t;
typedef struct { int16_t x; uint8_t lane, art; bool active, passed; } dodge_obstacle_t;
typedef struct {
    dodge_mode_t mode;
    unsigned selection, lane, score, best, speed, steps;
    uint8_t move_remainder;
    uint32_t rng;
    dodge_obstacle_t obstacles[DODGE_OBSTACLES];
} dodge_t;
void dodge_init(dodge_t *game, unsigned best, uint32_t seed);
void dodge_key(dodge_t *game, dodge_key_t key);
void dodge_step(dodge_t *game);
