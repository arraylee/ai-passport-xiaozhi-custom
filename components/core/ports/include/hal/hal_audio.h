// components/hal/include/hal/hal_audio.h
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef struct hal_audio_s hal_audio_t;
// read_echo supplied a complete frame, but older DMA input was dropped.
// The caller must discard its continuity-dependent filter/history state.
enum { HAL_AUDIO_ECHO_GAP = 1 };
// Exclusive duplex voice transfers use whole DMA blocks. 120 frames divide
// both negotiated 60ms packet sizes (960 at16k and1440 at24k).
enum { HAL_AUDIO_DUPLEX_FRAMES = 120 };

typedef struct {
    int  (*write)(hal_audio_t *self, const void *pcm, size_t n, size_t *written);
    int  (*read) (hal_audio_t *self, void *pcm, size_t n, size_t *got);
    void (*set_volume)(hal_audio_t *self, uint8_t percent);
    int  (*set_sample_rate)(hal_audio_t *self, uint32_t hz, uint8_t bits, uint8_t ch);
    int  (*set_out_mute)(hal_audio_t *self, bool mute);   // 可选,NULL 表示不支持
    int  (*shutdown)(hal_audio_t *self);                  // 可选:进入低功耗前关闭 codec/I2S
    /* Optional exclusive voice mode. write remains mono; read_echo returns
     * interleaved two-slot capture with the hardware DAC reference. */
    int (*set_echo_capture)(hal_audio_t *self, bool enable);
    int (*read_echo)(hal_audio_t *self, int16_t *stereo, size_t frames, size_t *got);
    /* True only after non-silent PCM has crossed the physical speaker output. */
    bool (*output_active)(hal_audio_t *self);
} hal_audio_api_t;

struct hal_audio_s { const hal_audio_api_t *api; void *impl; };

static inline bool hal_audio_output_active(hal_audio_t *a) {
    return a && a->api && a->api->output_active && a->api->output_active(a);
}

static inline int hal_audio_write(hal_audio_t *a, const void *p, size_t n, size_t *w) {
    return a->api->write(a, p, n, w);
}
static inline int hal_audio_read(hal_audio_t *a, void *p, size_t n, size_t *g) {
    return a->api->read(a, p, n, g);
}
static inline void hal_audio_set_volume(hal_audio_t *a, uint8_t percent) {
    a->api->set_volume(a, percent);
}
static inline int hal_audio_set_sample_rate(hal_audio_t *a, uint32_t hz, uint8_t bits, uint8_t ch) {
    return a->api->set_sample_rate(a, hz, bits, ch);
}
// 静音/取消静音输出(DAC)。实现可选:未实现时返回 0(视为无操作)。
static inline int hal_audio_set_out_mute(hal_audio_t *a, bool mute) {
    return a->api->set_out_mute ? a->api->set_out_mute(a, mute) : 0;
}
// 软件关闭音频硬件。实现可选；深睡唤醒会复位并重新初始化，无需提供 resume。
static inline int hal_audio_shutdown(hal_audio_t *a) {
    return (a && a->api->shutdown) ? a->api->shutdown(a) : 0;
}

static inline int hal_audio_set_echo_capture(hal_audio_t *a, bool enable) {
    return a && a->api->set_echo_capture ? a->api->set_echo_capture(a, enable) : -1;
}
static inline int hal_audio_read_echo(hal_audio_t *a, int16_t *stereo, size_t frames, size_t *got) {
    if (got) *got = 0;
    return a && a->api->read_echo ? a->api->read_echo(a, stereo, frames, got) : -1;
}
