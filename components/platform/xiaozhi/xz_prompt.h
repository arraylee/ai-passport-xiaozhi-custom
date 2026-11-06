#pragma once
#include "xiaozhi.h"
#include <stdatomic.h>
// Returns the number of PCM samples accepted by the HAL, or zero on interruption/error.
size_t xz_play_prompt(const xz_config_t *config, const atomic_bool *cancel,
                        const atomic_bool *end, const uint8_t *data, size_t size);
// Native 24 kHz signed little-endian PCM16, streamed in 240-byte blocks.
size_t xz_play_pcm_prompt(const xz_config_t *config, const atomic_bool *cancel,
                        const atomic_bool *end, const uint8_t *data, size_t size);
