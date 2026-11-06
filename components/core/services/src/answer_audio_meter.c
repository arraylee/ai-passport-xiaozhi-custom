#include "services/answer_audio_meter.h"

#include <limits.h>
#include <math.h>

int32_t ab_audio_frame_centidbfs(const int16_t *samples, size_t sample_count)
{
    if (samples == NULL || sample_count == 0) {
        return AB_AUDIO_FLOOR_CDBFS;
    }
    int64_t sum = 0;
    for (size_t index = 0; index < sample_count; ++index) {
        sum += samples[index];
    }
    const int32_t mean = (int32_t)(sum / (int64_t)sample_count);
    uint64_t sum_squares = 0;
    for (size_t index = 0; index < sample_count; ++index) {
        const int32_t centered = (int32_t)samples[index] - mean;
        sum_squares += (uint64_t)((int64_t)centered * centered);
    }
    if (sum_squares == 0) {
        return AB_AUDIO_FLOOR_CDBFS;
    }
    const double rms = sqrt((double)sum_squares / (double)sample_count);
    const double dbfs = 20.0 * log10(rms / 32768.0);
    int32_t centidbfs = (int32_t)lround(dbfs * 100.0);
    if (centidbfs < AB_AUDIO_FLOOR_CDBFS) {
        centidbfs = AB_AUDIO_FLOOR_CDBFS;
    }
    if (centidbfs > 0) {
        centidbfs = 0;
    }
    return centidbfs;
}

void ab_audio_meter_reset(ab_audio_meter_t *meter)
{
    if (meter == NULL) {
        return;
    }
    meter->summary.voiced_ms = 0;
    meter->summary.peak_centidbfs = AB_AUDIO_FLOOR_CDBFS;
    meter->summary.current_centidbfs = AB_AUDIO_FLOOR_CDBFS;
}

void ab_audio_meter_push(
    ab_audio_meter_t *meter,
    const int16_t *samples,
    size_t sample_count,
    uint32_t frame_ms)
{
    if (meter == NULL || samples == NULL || sample_count == 0) {
        return;
    }
    const int32_t level = ab_audio_frame_centidbfs(samples, sample_count);
    meter->summary.current_centidbfs = level;
    if (level > meter->summary.peak_centidbfs) {
        meter->summary.peak_centidbfs = level;
    }
    if (level >= AB_AUDIO_VOICE_THRESHOLD_CDBFS) {
        if (UINT32_MAX - meter->summary.voiced_ms < frame_ms) {
            meter->summary.voiced_ms = UINT32_MAX;
        } else {
            meter->summary.voiced_ms += frame_ms;
        }
    }
}

ab_audio_summary_t ab_audio_meter_snapshot(const ab_audio_meter_t *meter)
{
    const ab_audio_summary_t empty = {
        .voiced_ms = 0,
        .peak_centidbfs = AB_AUDIO_FLOOR_CDBFS,
        .current_centidbfs = AB_AUDIO_FLOOR_CDBFS,
    };
    return meter == NULL ? empty : meter->summary;
}

uint16_t ab_audio_level_units(int32_t centidbfs)
{
    const int32_t floor = -6000;
    const int32_t ceiling = -1200;
    if (centidbfs <= floor) {
        return 0;
    }
    if (centidbfs >= ceiling) {
        return 1000;
    }
    return (uint16_t)(((centidbfs - floor) * 1000) / (ceiling - floor));
}

bool ab_audio_is_valid(ab_audio_summary_t summary)
{
    return summary.voiced_ms >= AB_AUDIO_REQUIRED_VOICED_MS
        && summary.peak_centidbfs >= AB_AUDIO_PEAK_THRESHOLD_CDBFS;
}
