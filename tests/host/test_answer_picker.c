#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "services/answer_picker.h"
int main(void) {
    ab_answer_picker_t p;
    ab_answer_picker_init(&p);
    assert(ab_answer_picker_next(&p, 0, 1) == SIZE_MAX);
    assert(ab_answer_picker_next(NULL, 24, 1) == SIZE_MAX);
    for (unsigned n = 1; n <= 24; n++) {
        ab_answer_picker_init(&p);
        size_t history[5] = { SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX };
        for (unsigned i = 0; i < 400; i++) {
            size_t pick = ab_answer_picker_next(&p, n, i % 3); /* adversarial repeating RNG */
            assert(pick < n);
            unsigned avoid = n > 6 ? 5 : n > 1 ? 1 : 0;
            for (unsigned j = 0; j < avoid; j++) assert(pick != history[j]);
            for (unsigned j = 4; j; j--) history[j] = history[j - 1];
            history[0] = pick;
        }
    }
    puts("answer_picker: 9600 selections, recent exclusion and small corpus PASS");
}
