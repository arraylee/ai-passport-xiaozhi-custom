#include "xz_quiet.h"

void xz_quiet_begin_listening(xz_voice_pcm_t *buffer) {
  WebRtcVad_InitCore(&buffer->listen.vad);
  WebRtcVad_set_mode_core(&buffer->listen.vad, 2);
}

bool xz_quiet_feed(xz_quiet_t *clock, xz_voice_pcm_t *buffer) {
  const unsigned limit = XZ_QUIET_MS * (XZ_QUIET_RATE / 1000);
  for (size_t at = 0; at < 960; at += XZ_QUIET_SLICE) {
    if (WebRtcVad_CalcVad16khz(&buffer->listen.vad, buffer->listen.samples + at, XZ_QUIET_SLICE) > 0) {
      clock->quiet_samples = 0;
    } else if (clock->quiet_samples < limit) {
      clock->quiet_samples += XZ_QUIET_SLICE;
    }
  }
  return clock->quiet_samples >= limit;
}
