#pragma once

#include <stddef.h>
#include <stdint.h>

#define AB_ANSWER_RECENT_COUNT 5u

typedef struct {
    size_t recent[AB_ANSWER_RECENT_COUNT];
    size_t recent_count;
    size_t recent_cursor;
    size_t last;
} ab_answer_picker_t;

void ab_answer_picker_init(ab_answer_picker_t *picker);
size_t ab_answer_picker_next(
    ab_answer_picker_t *picker,
    size_t answer_count,
    uint32_t random_value);
