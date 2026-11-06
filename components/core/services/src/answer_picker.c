#include "services/answer_picker.h"

#include <stdbool.h>

void ab_answer_picker_init(ab_answer_picker_t *picker)
{
    if (picker == NULL) {
        return;
    }
    for (size_t index = 0; index < AB_ANSWER_RECENT_COUNT; ++index) {
        picker->recent[index] = SIZE_MAX;
    }
    picker->recent_count = 0;
    picker->recent_cursor = 0;
    picker->last = SIZE_MAX;
}

static bool excluded(
    const ab_answer_picker_t *picker,
    size_t answer_count,
    size_t candidate)
{
    if (answer_count <= 6) {
        return answer_count > 1 && candidate == picker->last;
    }
    for (size_t index = 0; index < picker->recent_count; ++index) {
        if (picker->recent[index] == candidate) {
            return true;
        }
    }
    return false;
}

static void remember(ab_answer_picker_t *picker, size_t answer)
{
    picker->last = answer;
    if (picker->recent_count < AB_ANSWER_RECENT_COUNT) {
        picker->recent[picker->recent_count++] = answer;
        return;
    }
    picker->recent[picker->recent_cursor] = answer;
    picker->recent_cursor =
        (picker->recent_cursor + 1u) % AB_ANSWER_RECENT_COUNT;
}

size_t ab_answer_picker_next(
    ab_answer_picker_t *picker,
    size_t answer_count,
    uint32_t random_value)
{
    if (picker == NULL || answer_count == 0) {
        return SIZE_MAX;
    }
    uint32_t state = random_value;
    size_t candidate = (size_t)(state % answer_count);
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (!excluded(picker, answer_count, candidate)) {
            remember(picker, candidate);
            return candidate;
        }
        state = state * 1664525u + 1013904223u;
        candidate = (size_t)(state % answer_count);
    }
    const size_t start = candidate;
    for (size_t offset = 0; offset < answer_count; ++offset) {
        candidate = (start + offset) % answer_count;
        if (!excluded(picker, answer_count, candidate)) {
            remember(picker, candidate);
            return candidate;
        }
    }
    remember(picker, start);
    return start;
}
