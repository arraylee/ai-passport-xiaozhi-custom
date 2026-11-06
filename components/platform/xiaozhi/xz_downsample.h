#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int16_t history[2][24];
    unsigned at, phase, factor;
} xz_downsample_t;
bool xz_downsample_init(xz_downsample_t *, unsigned input_rate);
/* Input and output pairs have one shared clock and identical FIR delay. */
bool xz_downsample_push(xz_downsample_t *,int16_t mic,int16_t reference,int16_t *out_mic,int16_t *out_ref);
