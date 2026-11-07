#include "xz_prompt.h"
#include "services/adpcm.h"
#include <string.h>
#include "xz_rice.h"

static size_t play_prompt(const xz_config_t *config, const atomic_bool *cancel,
                        const atomic_bool *end, const uint8_t *data, size_t size, bool pcm24) {
  bool packed=pcm24 && data && size>=12 && !memcmp(data,"RCE1",4);
  xz_rice_t rice={0};
  if(packed){rice.p=data+12;rice.end=data+size;rice.remaining=rice_u32(data+4);}
  if (!config->audio || !config->acquire_audio || !config->release_audio || !data || !size ||
      (pcm24 && !packed && size % sizeof(int16_t)) ||
      atomic_load(cancel) || atomic_load(end) || !config->acquire_audio(config->user)) return 0;
  hal_audio_t *audio = config->audio;
  adpcm_state_t decoder;
  adpcm_state_reset(&decoder);
  int16_t pcm[HAL_AUDIO_DUPLEX_FRAMES];
  size_t pos = packed?12:0, delivered=0;
  bool ok = hal_audio_set_sample_rate(audio, pcm24 ? 24000 : 16000, 16, 1) == 0 &&
            hal_audio_set_out_mute(audio, false) == 0;
  while (ok && pos < size && !atomic_load(cancel) && !atomic_load(end)) {
    size_t block = size - pos;
    size_t capacity = pcm24 ? sizeof(pcm) : HAL_AUDIO_DUPLEX_FRAMES / 2;
    if (block > capacity) block = capacity;
    size_t samples;
    if (packed) {
      samples=rice.remaining>HAL_AUDIO_DUPLEX_FRAMES?HAL_AUDIO_DUPLEX_FRAMES:rice.remaining;
      if(!xz_rice_block(&rice,pcm,samples)){ok=false;break;}
      block=(size_t)(rice.p-data)-pos;
    } else if (pcm24) {
      // Copy only one DMA block from Flash; no whole-recording allocation.
      memcpy(pcm, data + pos, block);
      samples = block / sizeof(int16_t);
    } else samples = (size_t)adpcm_decode(&decoder, data + pos, (int)block, pcm);
    size_t written = 0;
    ok = hal_audio_write(audio, pcm, samples * sizeof(*pcm), &written) == 0 &&
         written == samples * sizeof(*pcm);
    if (ok) {pos += block;delivered+=samples;}
  }
  // The asset ends with 100 ms of silence, longer than the device DMA queue.
  hal_audio_set_out_mute(audio, true);
  int closed = hal_audio_set_echo_capture(audio, false);
  config->release_audio(config->user);
  return ok && closed == 0 && pos == size && (!packed || !rice.remaining) ? delivered : 0;
}

size_t xz_play_prompt(const xz_config_t *config, const atomic_bool *cancel,
                        const atomic_bool *end, const uint8_t *data, size_t size) {
  return play_prompt(config, cancel, end, data, size, false);
}

size_t xz_play_pcm_prompt(const xz_config_t *config, const atomic_bool *cancel,
                        const atomic_bool *end, const uint8_t *data, size_t size) {
  return play_prompt(config, cancel, end, data, size, true);
}
