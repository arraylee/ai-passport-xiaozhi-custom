#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AB_AUDIO_VOICE_THRESHOLD_CDBFS (-4200)
#define AB_AUDIO_PEAK_THRESHOLD_CDBFS  (-3200)
#define AB_AUDIO_REQUIRED_VOICED_MS     300u
#define AB_AUDIO_FLOOR_CDBFS            (-9600)

typedef struct {
    uint32_t voiced_ms;
    int32_t peak_centidbfs;
    int32_t current_centidbfs;
} ab_audio_summary_t;

typedef struct {
    ab_audio_summary_t summary;
} ab_audio_meter_t;

/* Mean-centered frame RMS in centidBFS; silence/empty input returns the floor.
 * Shared by local sound experiences without changing answerbook thresholds. */
int32_t ab_audio_frame_centidbfs(const int16_t *samples, size_t sample_count);
void ab_audio_meter_reset(ab_audio_meter_t *meter);
void ab_audio_meter_push(
    ab_audio_meter_t *meter,
    const int16_t *samples,
    size_t sample_count,
    uint32_t frame_ms);
ab_audio_summary_t ab_audio_meter_snapshot(const ab_audio_meter_t *meter);
uint16_t ab_audio_level_units(int32_t centidbfs);
bool ab_audio_is_valid(ab_audio_summary_t summary);
