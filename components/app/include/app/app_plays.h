#pragma once
#include "hal/hal_audio.h"
#include "hal/hal_button.h"
#include "hal/hal_identity.h"
#include <stdbool.h>

typedef struct {
    hal_audio_t *audio;
    hal_identity_t *identity;
    // Called only by the play worker, outside the LVGL lock.
    bool (*acquire_audio)(void *user);
    void (*release_audio)(void *user);
    unsigned (*get_volume)(void *user);
    bool (*set_volume)(void *user, unsigned volume);
    unsigned (*get_brightness)(void *user);
    bool (*set_brightness)(void *user, unsigned brightness);
    bool (*preview_brightness)(void *user, unsigned brightness);
    unsigned (*get_sleep_minutes)(void *user);
    bool (*set_sleep_minutes)(void *user, unsigned minutes);
    void (*game_result)(uint32_t total, uint32_t best, uint32_t rounds, void *user);
    // Called under the LVGL lock when returning to the profile.
    void (*exit)(void *user);
    // Called under LVGL after the voice screen exists, before networking starts.
    void (*voice_opened)(void *user);
    void *user;
    // The Wi-Fi form calls this on a scan or submission before starting I/O.
    bool (*portal_activity)(void);
} app_plays_config_t;

bool app_plays_init(const app_plays_config_t *config);
bool app_plays_open(int best); // LVGL context/lock required; does no IO.
bool app_plays_open_xiaozhi(int best); // Same lock contract; queues normal startup.
bool app_plays_active(void);
bool app_plays_clicker_active(void);
bool app_plays_voice_busy(void);
// Local serial fixture: normal UI/worker startup, bounded to45seconds, then
// stop and return home. Reject other plays, active voice, sleep and duplicates.
bool app_plays_voice_check(void);
// Forward the complete raw gesture (including PRESS). Follow-up events from a
// press that started outside the current play page are deliberately consumed.
void app_plays_button(int index, hal_btn_event_t event);
// BLE callback only records an event; the worker owns QR navigation.
void app_plays_config_connected(void);
// Nonblocking flush handshake used before deep sleep; repeated calls are safe.
bool app_plays_prepare_sleep(void);
bool app_plays_commit_sleep(void);
void app_plays_cancel_sleep(void);
