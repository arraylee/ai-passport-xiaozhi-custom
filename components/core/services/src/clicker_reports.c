#include "services/clicker_reports.h"
#include <string.h>

void clicker_reports_init(clicker_reports_t *s) { memset(s, 0, sizeof(*s)); s->report_id = 1; }
bool clicker_reports_busy(const clicker_reports_t *s) { return s->phase != 0; }
const uint8_t *clicker_reports_current(const clicker_reports_t *s) { return s->current; }
const uint8_t *clicker_reports_mouse_current(const clicker_reports_t *s) { return s->mouse_current; }
const uint8_t *clicker_reports_touch_current(const clicker_reports_t *s) { return s->touch_current; }
uint8_t clicker_reports_id(const clicker_reports_t *s) { return s->report_id; }
bool clicker_reports_request(clicker_reports_t *s, clicker_action_t action, bool ready) {
    if (!ready || s->phase || s->disconnect_required || action < CLICKER_PREV || action > CLICKER_POINTER_TEST) return false;
    /* Explicit computer commands; the keyboard cannot observe slideshow state. */
    static const uint8_t codes[] = {0x4b, 0x4e, 0x3e, 0x29, 0x05};
    s->report_id = action == CLICKER_POINTER_TEST ? 2 : action >= CLICKER_TOUCH_PREV ? 3 : action >= CLICKER_PHONE_PREV ? 2 : 1;
    s->key = action == CLICKER_POINTER_TEST ? 2 : s->report_id == 3 ? (action == CLICKER_TOUCH_NEXT) :
             s->report_id == 2 ? (action == CLICKER_PHONE_NEXT) : codes[action];
    s->step = 0;
    s->aborted = false;
    s->release_attempts = 0;
    s->phase = s->report_id == 3 ? 5 : s->report_id == 2 ? 3 : 1;
    return true;
}
static void put_relative(uint8_t *out, int16_t value) {
    out[0] = (uint16_t)value & 0xff;
    out[1] = (uint16_t)value >> 8;
}
static void mouse_tick(clicker_reports_t *s, clicker_send_fn send, void *user) {
    uint8_t report[8] = {0};
    if (s->key == 2) {
        /* Explicit pointer diagnostic: one small motion, no click or wheel.
         * Relative HID units are not pixels; this does not calibrate position. */
        put_relative(report + 1, 24);
        put_relative(report + 3, 24);
        if (send(report, user)) s->sent++;
        else s->failed++;
        s->phase = 0;
        return;
    }
    /* AssistiveTouch: press, vertical drag, release, unpressed return.
     * Match the return's magnitudes and cadence; never home to a screen edge.
     * These are relative HID units, not pixels. Equal deltas cannot guarantee
     * equal screen motion under every host's pointer gain/clipping behavior.
     * Behavioral reference: Jieli keypage's iOS before sequences; see
     * docs/bluetooth-clicker.md. Android remains absolute touch. */
    if (s->phase == 3) {
        static const int16_t y[] = {0, 32, 48, 0, -32, -48, 0};
        report[0] = s->step < 3 ? 1 : 0;
        put_relative(report + 3, s->key ? -y[s->step] : y[s->step]);
    } /* phase 4 is an abort/exit release, with no movement. */
    bool releasing = s->mouse_current[0] && !report[0];
    if (!send(report, user)) {
        if (releasing) {
            if (++s->release_attempts >= 3) {
                if (!s->aborted) s->failed++;
                s->aborted = true;
                s->disconnect_required = true;
            }
        } else {
            s->failed++; s->aborted = true;
            s->phase = s->mouse_current[0] ? 4 : 0;
        }
        return;
    }
    /* Cache only button state. Relative motion must never replay on a read. */
    memset(s->mouse_current, 0, sizeof(s->mouse_current));
    s->mouse_current[0] = report[0];
    if (s->phase == 4 || ++s->step == 7) {
        s->phase = 0;
        if (!s->aborted) s->sent++;
    }
}
static void touch_tick(clicker_reports_t *s, clicker_send_fn send, void *user) {
    uint8_t report[8] = {0};
    memcpy(report, s->touch_current, sizeof(s->touch_current));
    if (s->phase == 5) {
        /* HID absolute domain 0..10000: x=50%, y=68%->32% (next), reversed
         * for previous. 12 intervals at 25 ms, no corner reset or mouse gain. */
        report[0] = 3; /* tip down and in range */
        put_relative(report + 1, 5000);
        put_relative(report + 3, s->key ? 6800 - s->step * 300 : 3200 + s->step * 300);
    } else report[0] = 0; /* lift at last accepted point, including cancellation */
    if (!send(report, user)) {
        if (s->phase == 6) {
            if (++s->release_attempts >= 3) {
                if (!s->aborted) s->failed++;
                s->aborted = true; s->disconnect_required = true;
            }
        } else {
            s->failed++; s->aborted = true;
            s->phase = s->touch_current[0] ? 6 : 0;
        }
        return;
    }
    /* Absolute GATT reads keep the current coordinate; they add no motion. */
    memcpy(s->touch_current, report, sizeof(s->touch_current));
    if (s->phase == 5) {
        if (++s->step == 13) s->phase = 6;
    } else {
        s->phase = 0;
        if (!s->aborted) s->sent++;
    }
}
void clicker_reports_tick(clicker_reports_t *s, clicker_send_fn send, void *user) {
    if (!s->phase || s->disconnect_required || !send) return;
    if (s->report_id == 2) { mouse_tick(s, send, user); return; }
    if (s->report_id == 3) { touch_tick(s, send, user); return; }
    uint8_t report[8] = {0};
    if (s->phase == 1) report[2] = s->key;
    if (!send(report, user)) {
        if (s->phase == 1) { s->failed++; s->phase = 0; }
        else if (++s->release_attempts >= 3) { s->failed++; s->disconnect_required = true; }
        return;
    }
    memcpy(s->current, report, sizeof(s->current));
    if (s->phase == 1) { s->phase = 2; return; }
    s->phase = 0;
    s->sent++;
}
void clicker_reports_cancel(clicker_reports_t *s) {
    if (s->report_id == 3 && s->phase) {
        s->aborted = true;
        s->phase = s->touch_current[0] ? 6 : 0;
        return;
    }
    if (s->report_id == 2 && s->phase) {
        s->aborted = true;
        s->phase = s->mouse_current[0] ? 4 : 0;
        return;
    }
    /* An unsent key can be discarded; a sent key MUST receive release. */
    if (s->phase == 1) { s->phase = 0; }
}
void clicker_reports_disconnected(clicker_reports_t *s) {
    s->phase = 0;
    s->disconnect_required = false;
    s->release_attempts = 0;
    memset(s->current, 0, sizeof(s->current));
    memset(s->mouse_current, 0, sizeof(s->mouse_current));
    memset(s->touch_current, 0, sizeof(s->touch_current));
}
