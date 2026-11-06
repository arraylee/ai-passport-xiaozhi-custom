#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/clicker_reports.h"
/* Appended actions, leaving keyboard and relative-pointer ABI untouched. */
#define TOUCH_PREV ((clicker_action_t)7)
#define TOUCH_NEXT ((clicker_action_t)8)
static uint8_t packets[64][8];
static unsigned count, failures;
static bool send_packet(const uint8_t p[8], void *user) {
    (void)user;
    if (failures) { --failures; return false; }
    assert(count < 64); memcpy(packets[count++], p, 8); return true;
}
static int coord(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void swipe(clicker_action_t action) {
    clicker_reports_t s; clicker_reports_init(&s); count = failures = 0;
    assert(!clicker_reports_request(&s, action, false));
    assert(clicker_reports_request(&s, action, true));
    assert(clicker_reports_id(&s) == 3);
    while (clicker_reports_busy(&s)) {
        clicker_reports_tick(&s, send_packet, NULL);
        assert(count < 40);
    }
    assert(s.sent == 1 && s.failed == 0 && count >= 8);
    int first_y = coord(packets[0] + 3), last_y = first_y;
    /* Map actual encoded reports to different phone dimensions: the pressed
     * path must stay in the centre, away from all system/app edge controls. */
    const int screens[][2] = {{720,1600},{1080,2400},{1170,2532},{2480,2248}};
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *p = packets[i];
        int x = coord(p+1), y = coord(p+3);
        assert(p[5] == 0 && p[6] == 0 && p[7] == 0);
        assert(x == coord(packets[0]+1));
        assert(action == TOUCH_NEXT ? y <= last_y : y >= last_y);
        for (unsigned screen = 0; screen < 4; ++screen) {
            int px = x*screens[screen][0]/10000, py = y*screens[screen][1]/10000;
            assert(px >= screens[screen][0]*45/100 && px <= screens[screen][0]*55/100);
            assert(py >= screens[screen][1]*30/100 && py <= screens[screen][1]*70/100);
        }
        assert(i+1 == count ? p[0] == 0 : (p[0]&3) == 3);
        if (i+1 == count) assert(y == last_y); /* release at endpoint, never jump to corner */
        last_y = y;
    }
    assert(action == TOUCH_NEXT ? first_y-last_y >= 3000 : last_y-first_y >= 3000);
    for (unsigned i=0;i<8;i++) assert(clicker_reports_current(&s)[i] == 0);
}
int main(void) {
    swipe(TOUCH_PREV); swipe(TOUCH_NEXT);
    unsigned total = count;
    /* Every cancellation and send-failure boundary must release contact at the
     * last accepted coordinate. A failed release retries, then disconnects. */
    for (unsigned cut = 0; cut < total; ++cut) for (unsigned fail = 0; fail < 2; ++fail) {
        clicker_reports_t s; clicker_reports_init(&s); count = failures = 0;
        assert(clicker_reports_request(&s, TOUCH_NEXT, true));
        for (unsigned i=0;i<cut;i++) clicker_reports_tick(&s,send_packet,NULL);
        if (fail) { failures=1; clicker_reports_tick(&s,send_packet,NULL); }
        else clicker_reports_cancel(&s);
        while(clicker_reports_busy(&s)) clicker_reports_tick(&s,send_packet,NULL);
        if (count) assert(packets[count-1][0] == 0);
        if (cut) assert(coord(packets[count-1]+3) == coord(packets[cut-1]+3));
        assert(s.sent == (fail && cut+1 == total ? 1u : 0u));
        assert(s.failed == (fail && cut+1 < total ? 1u : 0u));
    }
    clicker_reports_t s; clicker_reports_init(&s); count=failures=0;
    assert(clicker_reports_request(&s,TOUCH_NEXT,true));
    for(unsigned i=1;i<total;i++) clicker_reports_tick(&s,send_packet,NULL);
    failures=3;
    for(unsigned i=0;i<3;i++) clicker_reports_tick(&s,send_packet,NULL);
    assert(s.disconnect_required && s.failed == 1 && s.sent == 0);
    clicker_reports_disconnected(&s); assert(!clicker_reports_busy(&s));
    puts("Absolute touch: central screen-safe path, direction, no keyboard, all release boundaries PASS");
}
