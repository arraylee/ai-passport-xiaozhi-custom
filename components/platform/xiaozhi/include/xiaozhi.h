#pragma once
#include "hal/hal_audio.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum {
  XZ_OFF,
  XZ_WIFI,
  XZ_SETUP,
  XZ_CLOUD,
  XZ_BIND,
  XZ_READY,
  XZ_LISTEN,
  XZ_THINK,
  XZ_SPEAK,
  XZ_ERROR,
  XZ_STANDBY,
  XZ_WAKING,
  XZ_FAREWELL,
  XZ_BOOT,
  XZ_WELCOME,
  XZ_RADIO
} xz_phase_t;
// Preserved when phase becomes ERROR, so a cloud/voice failure cannot be
// presented as lost Wi-Fi credentials. WIFI is the zero-initialized stage.
typedef enum { XZ_STAGE_WIFI, XZ_STAGE_CLOUD, XZ_STAGE_BIND,
               XZ_STAGE_VOICE, XZ_STAGE_DEVICE } xz_stage_t;
typedef struct {
  xz_phase_t phase;
  xz_stage_t stage;
  unsigned avatar, revision, emotion;
  bool restart_required, wifi_saved;
  bool natural_interrupt, output_active;
  unsigned volume;
  char detail[320], code[24], hotspot[32], password[12];
} xz_view_t;
typedef struct {
  hal_audio_t *audio;
  bool (*acquire_audio)(void *);
  void (*release_audio)(void *);
  bool (*pause_radio)(void);
  bool (*resume_radio)(void);
  unsigned (*get_volume)(void *);
  bool (*set_volume)(void *, unsigned);
  void *user;
  unsigned (*get_brightness)(void *);
  bool (*set_brightness)(void *, unsigned);
  bool (*portal_activity)(void);
} xz_config_t;
// Cold-boot only, before the networking worker starts. Streams one DMA block.
unsigned xiaozhi_play_startup_sound(void);
void xiaozhi_init(const xz_config_t *config);
bool xiaozhi_start(bool configure_wifi);
// Call start/service only from the plays worker, without the LVGL lock.
// service reclaims the finished voice task before rebuilding Bluetooth.
void xiaozhi_service(void);
void xiaozhi_stop(void); /* nonblocking cancellation, safe under LVGL lock */
bool xiaozhi_stopped(
    void); /* true only after all network/audio resources released */
void xiaozhi_talk(void); /* End audio and park on the online portrait. */
void xiaozhi_radio_step(int direction);
void xiaozhi_welcome_shown(void);
void xiaozhi_begin_setup(void);
bool xiaozhi_output_active(void);
void xiaozhi_resume(void); /* Start a new conversation from online standby. */
#ifdef PASSPORT_TEST_SERVICES
// Bench-only: fixed synthetic requests; absent from production firmware.
bool xiaozhi_test_service(unsigned kind);
bool xiaozhi_test_take_network_only(void);
#endif
void xiaozhi_interrupt(void); /* stop the reply and keep this conversation open */
bool xiaozhi_set_natural_interrupt(bool enabled);
bool xiaozhi_set_volume(unsigned volume);
void xiaozhi_avatar(unsigned index);
void xiaozhi_snapshot(xz_view_t *view);
typedef struct {
  unsigned frames16, avg16_us, peak16_us, frames24, avg24_us, peak24_us;
  unsigned cpu_mhz, stack_free;
} xz_dsp_stats_t;
// Factory console holds UI/input exclusion; voice must be fully stopped.
// Uses generated samples only. Never acquires audio or changes the voice view.
bool xiaozhi_dsp_check(xz_dsp_stats_t *out);
