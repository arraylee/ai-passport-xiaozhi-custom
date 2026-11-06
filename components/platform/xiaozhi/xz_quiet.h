#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "webrtc/common_audio/vad/vad_core.h"

enum { XZ_QUIET_RATE = 16000, XZ_QUIET_SLICE = 480, XZ_QUIET_MS = 30000 };
typedef struct { unsigned quiet_samples; } xz_quiet_t;
// The decoder needs 1440 PCM samples at 24 kHz. The encoder needs only 960 at
// 16 kHz. Its VAD fits in the unused tail, and is reinitialized after playback.
typedef union {
  int16_t playback[1440];
  struct { int16_t samples[960]; VadInstT vad; } listen;
} xz_voice_pcm_t;
_Static_assert(sizeof(xz_voice_pcm_t) == 1440 * sizeof(int16_t), "VAD must fit the existing PCM buffer");
void xz_quiet_begin_listening(xz_voice_pcm_t *buffer);
bool xz_quiet_feed(xz_quiet_t *clock, xz_voice_pcm_t *buffer);
