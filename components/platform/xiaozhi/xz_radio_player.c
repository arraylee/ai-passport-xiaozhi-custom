#include "esp_mp3_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "xiaozhi.h"
#include "xz_services.h"
#include "xz_codec_memory.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "xz_radio_player.h"
static size_t frame_bytes(const uint8_t *p) {
  static const uint16_t v1[] = {0,   32,  40,  48,  56,  64,  80,  96,
                                112, 128, 160, 192, 224, 256, 320, 0};
  static const uint16_t v2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
  static const unsigned rates[] = {44100, 48000, 32000};
  unsigned ver = (p[1] >> 3) & 3, sr = (p[2] >> 2) & 3;
  if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0 || ver == 1 || (p[1] & 6) != 2 || sr == 3)
    return 0;
  unsigned bitrate = (ver == 3 ? v1 : v2)[p[2] >> 4];
  if (!bitrate)
    return 0;
  unsigned rate = rates[sr] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2);
  return (ver == 3 ? 144000u : 72000u) * bitrate / rate + ((p[2] >> 1) & 1);
}
typedef struct {
  const xz_config_t *config;
  void *decoder;
  uint8_t input[1536];
  int16_t pcm[2304];
  int16_t output[HAL_AUDIO_DUPLEX_FRAMES];
  unsigned output_used, fade;
  unsigned rate, bad, skipped, frames;
  uint32_t decode_peak, write_peak;
  uint64_t decode_us, write_us;
  size_t used;
  bool owned;
  atomic_bool failed, network_done, audio_done;
  atomic_uint read_at, write_at;
  uint8_t ring[8192];
  unsigned underruns, stack_free, output_samples;
#ifdef PASSPORT_TEST_SERVICES
  int64_t network_report;
#endif
} player_t;
static unsigned capacity(const player_t *p) { return sizeof p->ring; }
static uint8_t *ring_at(player_t *p, unsigned position) {
  return p->ring + (position & (capacity(p) - 1u));
}
static bool write_pcm(player_t *p, size_t samples) {
  for (size_t i = 0; i < samples; ++i) {
    int value = p->pcm[i];
    if (p->fade) { value = value * (120 - p->fade) / 120; --p->fade; }
    p->output[p->output_used++] = (int16_t)value;
    if (p->output_used == HAL_AUDIO_DUPLEX_FRAMES) {
      if (xz_online_cancelled()) return false;
      size_t written = 0;
      int64_t start = esp_timer_get_time();
      if (hal_audio_write(p->config->audio, p->output, sizeof p->output, &written) ||
          written != sizeof p->output) return false;
      uint32_t elapsed = esp_timer_get_time() - start;
      p->write_us += elapsed;
      if (elapsed > p->write_peak) p->write_peak = elapsed;
      p->output_used = 0;
      p->output_samples += HAL_AUDIO_DUPLEX_FRAMES;
    }
  }
  return true;
}
static bool decode(player_t *p, size_t length) {
  // TLS handshake has already finished. The largest decoder buffer was
  // reserved before connecting; smaller state can now use handshake memory.
  if (!p->decoder && esp_mp3_dec_open(NULL, 0, &p->decoder) != ESP_AUDIO_ERR_OK)
    return false;
  esp_audio_dec_in_raw_t in = {.buffer = p->input, .len = length};
  esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)p->pcm, .len = sizeof p->pcm};
  esp_audio_dec_info_t info = {0};
  int64_t started = esp_timer_get_time();
  esp_audio_err_t rc = esp_mp3_dec_decode(p->decoder, &in, &out, &info);
  uint32_t elapsed = esp_timer_get_time() - started;
  p->decode_us += elapsed;
  if (elapsed > p->decode_peak) p->decode_peak = elapsed;
  if (rc == ESP_AUDIO_ERR_MEM_LACK)
    return false;
  /* Joining a live MPEG bit reservoir mid-stream may discard initial frames. */
  if (rc != ESP_AUDIO_ERR_OK || !out.decoded_size)
    return ++p->bad <= 16;
  if (out.decoded_size > sizeof p->pcm || info.bits_per_sample != 16 ||
      (info.channel != 1 && info.channel != 2) || out.decoded_size % (2 * info.channel) ||
      info.sample_rate < 8000 || info.sample_rate > 48000)
    return false;
  if (!p->owned) {
    if (!p->config->acquire_audio(p->config->user))
      return false;
    p->owned = true;
  }
  hal_audio_t *audio = p->config->audio;
  if (p->rate != info.sample_rate) {
    if (hal_audio_set_sample_rate(audio, info.sample_rate, 16, 1) ||
        hal_audio_set_out_mute(audio, false))
      return false;
    p->rate = info.sample_rate;
    p->output_used = 0;
    p->fade = 120;
  }
  size_t samples = out.decoded_size / (2 * info.channel);
  if (info.channel == 2)
    for (size_t i = 0; i < samples; i++)
      p->pcm[i] = (int16_t)(((int)p->pcm[2 * i] + p->pcm[2 * i + 1]) / 2);
  if (xz_online_cancelled())
    return false;
  if (!write_pcm(p, samples)) return false;
  p->bad = p->skipped = 0;
  p->frames++;
  return true;
}
static bool feed(const char *data, size_t n, void *ctx) {
  player_t *p = ctx;
  while (n && !xz_online_cancelled()) {
    size_t add = sizeof p->input - p->used;
    if (add > n)
      add = n;
    memcpy(p->input + p->used, data, add);
    p->used += add;
    data += add;
    n -= add;
    size_t pos = 0, length = 0;
    while (p->used - pos >= 4 && !(length = frame_bytes(p->input + pos)))
      pos++;
    if (pos) {
      p->skipped += pos;
      p->used -= pos;
      memmove(p->input, p->input + pos, p->used);
    }
    if (p->skipped > 32768 || length > sizeof p->input) {
      p->failed = true;
      return false;
    }
    if (length && p->used >= length) {
      if (!decode(p, length)) {
        p->failed = true;
        return false;
      }
      p->used -= length;
      memmove(p->input, p->input + length, p->used);
      /* Drain already buffered complete frames even at the end of a network chunk. */
      while (p->used >= 4 && (length = frame_bytes(p->input)) && p->used >= length &&
             !xz_online_cancelled()) {
        if (!decode(p, length)) {
          p->failed = true;
          return false;
        }
        p->used -= length;
        memmove(p->input, p->input + length, p->used);
      }
    }
  }
  return !xz_online_cancelled();
}
static void consume(void *arg) {
  player_t *p = arg;
  bool registered = xz_codec_memory_radio_enter();
  if (!registered) atomic_store(&p->failed, true);
  uint8_t chunk[512];
  bool buffering = true, first_fill = true;
  while (!atomic_load(&p->failed) && !xz_online_cancelled()) {
    unsigned read = atomic_load_explicit(&p->read_at, memory_order_relaxed);
    unsigned available = atomic_load_explicit(&p->write_at, memory_order_acquire) - read;
    bool finished = atomic_load_explicit(&p->network_done, memory_order_acquire);
    if (!available && finished) break;
    if (!available && !buffering) {
      ++p->underruns; buffering = true; p->fade = HAL_AUDIO_DUPLEX_FRAMES;
      p->output_used = 0; // Never resume with an unfaded partial block after silence.
    }
    unsigned prefill = first_fill ? capacity(p) * 3u / 4u : capacity(p) / 2u;
    if ((buffering && available < prefill && !finished) || !available) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    buffering = first_fill = false;
    size_t n = available < sizeof chunk ? available : sizeof chunk;
    size_t pos = read % sizeof p->ring, first = sizeof p->ring - pos;
    if (first > n) first = n;
    memcpy(chunk, ring_at(p, read), first);
    memcpy(chunk + first, ring_at(p, read + first), n - first);
    atomic_store_explicit(&p->read_at, read + n, memory_order_release);
    if (!feed((const char *)chunk, n, p)) break;
  }
  if (p->owned) {
    hal_audio_set_out_mute(p->config->audio, true);
    hal_audio_set_echo_capture(p->config->audio, false);
    p->config->release_audio(p->config->user);
  }
  if (p->decoder) esp_mp3_dec_close(p->decoder);
  if (registered) xz_codec_memory_radio_leave();
  p->stack_free = uxTaskGetStackHighWaterMark(NULL);
  atomic_store_explicit(&p->audio_done, true, memory_order_release);
  vTaskSuspend(NULL); // The owner joins before deleting this task and its stack.
}
static bool receive(const char *data, size_t n, void *arg) {
  player_t *p = arg;
  while (n && !xz_online_cancelled() && !atomic_load(&p->failed) && !atomic_load(&p->audio_done)) {
    unsigned write = atomic_load_explicit(&p->write_at, memory_order_relaxed);
    unsigned free = capacity(p) - (write - atomic_load_explicit(&p->read_at, memory_order_acquire));
    if (!free) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    size_t pos = write % sizeof p->ring, add = sizeof p->ring - pos;
    if (add > n) add = n;
    if (add > free) add = free;
    memcpy(ring_at(p, write), data, add);
    atomic_store_explicit(&p->write_at, write + add, memory_order_release);
    data += add; n -= add;
  }
#ifdef PASSPORT_TEST_SERVICES
  int64_t now = esp_timer_get_time();
  if (now - p->network_report >= 10000000) {
    p->network_report = now;
    unsigned write = atomic_load(&p->write_at), read = atomic_load(&p->read_at);
    ESP_LOGI("xiaozhi", "radio_progress=1 received=%u consumed=%u queued=%u", write, read, write-read);
  }
#endif
  return !n && !xz_online_cancelled() && !atomic_load(&p->failed) && !atomic_load(&p->audio_done);
}
static bool stream_once(const xz_config_t *config, const xz_station_t *station, bool *retry) {
  *retry = false;
  if (!config->audio || !config->acquire_audio || !config->release_audio || !station)
    return false;
  // The voice worker already owns a 28 KiB stack. Its Opus/ASR frames have
  // returned before this function; reuse that storage for PCM and the queue.
  player_t state = {.config = config};
  player_t *p = &state;
  // Keep the compressed FIFO on the existing stack. The larger TCP window
  // absorbs packets in flight; a second heap FIFO would compete with those
  // packets and the decoder without improving a constrained TCP window.
  TaskHandle_t audio_task = NULL;
  if (xTaskCreate(consume, "radio_audio", 6144, p, 7, &audio_task) != pdPASS) return false;
  ESP_LOGI("xiaozhi", "radio_capacity=%u", capacity(p));
  bool http_ok = xz_http_visit(station->url, receive, p);
  atomic_store_explicit(&p->network_done, true, memory_order_release);
  while (!atomic_load_explicit(&p->audio_done, memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(2));
  vTaskDelete(audio_task);
  bool cancelled = xz_online_cancelled();
  bool failed = atomic_load(&p->failed) && !cancelled;
  bool ok = http_ok && !failed && p->frames && !cancelled;
  *retry = !http_ok && !failed && !cancelled;
  ESP_LOGI("xiaozhi","radio_stream=%u frames=%u rate=%u failed=%u cancelled=%u",ok,p->frames,p->rate,failed,cancelled);
  ESP_LOGI("xiaozhi", "radio_timing=1 decode_ms=%u write_ms=%u decode_peak_us=%u write_peak_us=%u",
           (unsigned)(p->decode_us / 1000), (unsigned)(p->write_us / 1000),p->decode_peak,p->write_peak);
  ESP_LOGI("xiaozhi", "radio_buffer=1 underruns=%u samples=%u audio_stack_free=%u network_stack_free=%u",
           p->underruns,p->output_samples,p->stack_free,(unsigned)uxTaskGetStackHighWaterMark(NULL));
  return ok;
}
#ifdef PASSPORT_TEST_SERVICES
typedef struct { int64_t start, last; size_t bytes; unsigned index; } network_probe_t;
static bool network_probe_receive(const char *data, size_t n, void *ctx) {
  (void)data;
  network_probe_t *p = ctx;
  p->bytes += n;
  int64_t now = esp_timer_get_time();
  if (now - p->last > 2000000) {
    ESP_LOGI("xiaozhi", "radio_probe_progress=%u bytes=%u elapsed_ms=%u", p->index,
             (unsigned)p->bytes, (unsigned)((now - p->start) / 1000));
    p->last = now;
  }
  return esp_timer_get_time() - p->start < 20000000 && p->bytes < 512000;
}
#endif
bool xz_radio_stream(const xz_config_t *config, const xz_station_t *station) {
#ifdef PASSPORT_TEST_SERVICES
  if (xiaozhi_test_take_network_only()) {
    const char *urls[] = {station->url, "https://lhttp.qingting.fm/live/1271/64k.mp3",
                          "http://lhttp.qingting.fm/live/15318317/64k.mp3"};
    bool result = true;
    for (unsigned i = 0; i < 3 && !xz_online_cancelled(); ++i) {
      network_probe_t p = {.start = esp_timer_get_time(), .index = i};
      bool ok = xz_http_visit(urls[i], network_probe_receive, &p);
      ESP_LOGI("xiaozhi", "radio_probe=%u index=%u bytes=%u elapsed_ms=%u", ok, i, (unsigned)p.bytes,
               (unsigned)((esp_timer_get_time() - p.start) / 1000));
      result &= ok;
    }
    return result;
  }
#endif
  for (unsigned attempt = 0; attempt < 3 && !xz_online_cancelled(); ++attempt) {
    bool retry = false;
    bool ok = stream_once(config, station, &retry);
    if (ok || !retry || attempt == 2) return ok;
    ESP_LOGI("xiaozhi", "radio_reconnect=%u", attempt + 1);
    // The completed attempt has already joined its task and freed every
    // decoder/socket allocation. Down/next can cancel the short back-off.
    for (unsigned i = 0; i < 12 && !xz_online_cancelled(); ++i) vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}
