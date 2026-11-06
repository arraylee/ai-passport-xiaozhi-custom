#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { CLICKER_PREV, CLICKER_NEXT, CLICKER_START, CLICKER_END, CLICKER_BLANK,
    CLICKER_PHONE_PREV, CLICKER_PHONE_NEXT,
    CLICKER_TOUCH_PREV, CLICKER_TOUCH_NEXT,
    CLICKER_POINTER_TEST,
    CLICKER_PLAY = CLICKER_START /* source compatibility; always sends F5 */ } clicker_action_t;
typedef bool (*clicker_send_fn)(const uint8_t report[8], void *user);
/* Owned by the BLE host. No allocations, blocking calls, or platform types. */
typedef struct {
    uint8_t key, phase, release_attempts;
    uint8_t current[8];
    uint8_t mouse_current[6], report_id, step;
    uint8_t touch_current[5];
    bool aborted;
    bool disconnect_required;
    uint32_t sent, failed;
} clicker_reports_t;
void clicker_reports_init(clicker_reports_t *state);
bool clicker_reports_request(clicker_reports_t *state, clicker_action_t action, bool ready);
void clicker_reports_tick(clicker_reports_t *state, clicker_send_fn send, void *user);
void clicker_reports_cancel(clicker_reports_t *state);
void clicker_reports_disconnected(clicker_reports_t *state);
bool clicker_reports_busy(const clicker_reports_t *state);
const uint8_t *clicker_reports_current(const clicker_reports_t *state);
const uint8_t *clicker_reports_mouse_current(const clicker_reports_t *state);
const uint8_t *clicker_reports_touch_current(const clicker_reports_t *state);
uint8_t clicker_reports_id(const clicker_reports_t *state);
