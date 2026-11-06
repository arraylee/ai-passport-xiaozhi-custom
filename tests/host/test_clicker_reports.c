#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/clicker_reports.h"

static unsigned fail_count, sends;
static uint8_t reports[32][8];
static bool send_report(const uint8_t data[8], void *u) {
    (void)u;
    if (fail_count) { fail_count--; return false; }
    assert(sends < 32); memcpy(reports[sends++], data, 8); return true;
}
int main(void) {
    clicker_reports_t c;
    clicker_reports_init(&c);
    uint8_t zero[8] = {0};
    assert(!memcmp(clicker_reports_current(&c), zero, 8));
    assert(!clicker_reports_request(&c, CLICKER_NEXT, false));
    assert(!clicker_reports_request(&c, (clicker_action_t)99, true));
    assert(clicker_reports_request(&c, CLICKER_NEXT, true));
    assert(!clicker_reports_request(&c, CLICKER_PREV, true)); /* cannot overwrite a held key */
    clicker_reports_tick(&c, send_report, NULL);
    assert(sends == 1 && reports[0][2] == 0x4e);
    assert(clicker_reports_current(&c)[2] == 0x4e);
    assert(c.sent == 0);
    fail_count = 2;
    clicker_reports_tick(&c, send_report, NULL);
    clicker_reports_tick(&c, send_report, NULL);
    assert(clicker_reports_busy(&c)); /* release must be retried */
    clicker_reports_tick(&c, send_report, NULL);
    assert(sends == 2 && !memcmp(reports[1], zero, 8));
    assert(!memcmp(clicker_reports_current(&c), zero, 8));
    assert(c.sent == 1 && !clicker_reports_busy(&c));
    assert(clicker_reports_request(&c, CLICKER_PREV, true));
    fail_count = 1;
    clicker_reports_tick(&c, send_report, NULL);
    assert(c.sent == 1 && c.failed == 1 && !clicker_reports_busy(&c));
    assert(!memcmp(clicker_reports_current(&c), zero, 8));
    assert(clicker_reports_request(&c, CLICKER_START, true));
    clicker_reports_tick(&c, send_report, NULL);
    assert(reports[2][2] == 0x3e); /* F5 */
    assert(clicker_reports_current(&c)[2] == 0x3e);
    clicker_reports_cancel(&c); /* leaving mode still owes key-up */
    assert(clicker_reports_busy(&c));
    clicker_reports_tick(&c, send_report, NULL);
    assert(!memcmp(reports[3], zero, 8));
    assert(clicker_reports_request(&c, CLICKER_START, true));
    clicker_reports_tick(&c, send_report, NULL);
    assert(reports[4][2] == 0x3e); /* F5 again: never guess host slideshow state */
    clicker_reports_tick(&c, send_report, NULL);
    assert(clicker_reports_request(&c, CLICKER_END, true));
    clicker_reports_tick(&c, send_report, NULL);
    assert(reports[6][2] == 0x29); /* Explicit Esc */
    clicker_reports_tick(&c, send_report, NULL);
    assert(clicker_reports_request(&c, CLICKER_END, true));
    clicker_reports_tick(&c, send_report, NULL);
    assert(reports[8][2] == 0x29); /* Repeated End remains Esc */
    clicker_reports_tick(&c, send_report, NULL);
    assert(clicker_reports_request(&c, CLICKER_BLANK, true));
    clicker_reports_tick(&c, send_report, NULL);
    assert(reports[10][2] == 0x05); /* B */
    fail_count=10;
    for (unsigned i=0;i<3;i++) clicker_reports_tick(&c, send_report, NULL);
    assert(c.disconnect_required); /* stuck-key path terminates BLE link */
    clicker_reports_disconnected(&c);
    assert(!c.disconnect_required && !clicker_reports_busy(&c));
    assert(!memcmp(clicker_reports_current(&c), zero, 8));
    puts("clicker_reports: key pairs, release retries, no overwrite and disconnect PASS");
}
