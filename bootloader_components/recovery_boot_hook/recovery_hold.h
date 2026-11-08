#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t started, pressed_at, released_at;
    bool pressing, releasing;
} recovery_hold_t;
/* 0: keep sampling, 1: enter Recovery, -1: boot the application. */
static inline int recovery_hold_sample(recovery_hold_t *s, uint32_t now, bool pressed)
{
    if (pressed) {
        if (!s->pressing) { s->pressing=true; s->pressed_at=now; }
        s->releasing=false;
        if (now-s->pressed_at>=200) return 1;
    } else if (s->pressing) {
        if (!s->releasing) { s->releasing=true; s->released_at=now; }
        if (now-s->released_at>=50) { s->pressing=false; s->releasing=false; }
    }
    if (!s->pressing && now-s->started>=1200) return -1;
    return 0;
}
