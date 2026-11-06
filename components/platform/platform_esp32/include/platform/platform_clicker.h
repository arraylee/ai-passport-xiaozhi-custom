#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "services/clicker_reports.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PLATFORM_CLICKER_OFF,
    PLATFORM_CLICKER_PAIRING,
    PLATFORM_CLICKER_CONNECTING,
    PLATFORM_CLICKER_READY,
    PLATFORM_CLICKER_UNAVAILABLE,
    PLATFORM_CLICKER_SWITCHING,
} platform_clicker_status_t;

typedef struct {
    platform_clicker_status_t status;
    bool keyboard_ready, mouse_ready, touch_ready;
    uint32_t sent;
    uint32_t failed;
    char name[32];
} platform_clicker_snapshot_t;

/* Runtime calls are non-blocking. BLE work is marshalled onto the NimBLE host. */
void platform_clicker_set_active(bool active);
bool platform_clicker_send(clicker_action_t action);
/* Release HID input, disconnect, forget only the current HID peer, then advertise.
 * Returns request acceptance, not completion or proof another computer paired. */
bool platform_clicker_change_computer(void);
void platform_clicker_snapshot(platform_clicker_snapshot_t *out);
/* Cache a real 0..100 battery sample; safe before BLE init and from timer tasks.
 * Invalid readings preserve the last sample. Never performs I2C from the host. */
void platform_clicker_set_battery(int percent);

#ifdef __cplusplus
}
#endif
