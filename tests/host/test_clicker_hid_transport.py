"""Exercise the production notify boundary and parse its HID report map."""
import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_clicker_platform import function, SOURCE, ROOT

BOUNDARY = r'''
#include <assert.h>
#include <string.h>
#include "services/clicker_reports.h"
#include "services/clicker_gate.h"
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define BLE_HS_CONN_HANDLE_NONE 0xffff
static bool s_host_active=true, s_exit_pending, s_conn_closing, s_connected=true, s_encrypted=true;
static bool s_subscribed, s_mouse_subscribed=true, s_touch_subscribed=true, s_shutting_down;
static uint16_t s_conn_handle=7, s_report_handle=12, s_mouse_report_handle=16, s_touch_report_handle=20;
static uint32_t s_report_generation;
static clicker_reports_t s_reports;
static clicker_gate_t s_gate;
struct os_mbuf { uint8_t data[8]; size_t len; };
static struct os_mbuf mbuf;
static unsigned notifications;
static uint16_t notified_handle;
static bool alloc_fail, notify_fail;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, size_t len) {
    if (alloc_fail) return NULL;
    assert(len <= 8); memcpy(mbuf.data, data, len); mbuf.len=len; return &mbuf;
}
static int ble_gatts_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om) {
    assert(conn==7 && om==&mbuf); notified_handle=handle; ++notifications;
    return notify_fail ? 1 : 0;
}
'''
CASES = r'''
int main(void) {
    clicker_reports_init(&s_reports); clicker_gate_init(&s_gate);
    clicker_gate_set_active(&s_gate, true); s_report_generation=s_gate.generation;
    uint8_t movement[8]={1,0,0,0xe0,0xff,0}, zero[8]={0};
    assert(clicker_reports_request(&s_reports, CLICKER_POINTER_TEST, true));
    clicker_reports_tick(&s_reports, send_report, NULL);
    const uint8_t probe[6] = {0,24,0,24,0,0};
    assert(notified_handle==16 && mbuf.len==6 && !memcmp(mbuf.data,probe,6));
    assert(clicker_reports_request(&s_reports, CLICKER_PHONE_NEXT, true));
    clicker_reports_tick(&s_reports, send_report, NULL);
    assert(notified_handle==16 && mbuf.len==6 && mbuf.data[0]==1);
    clicker_reports_tick(&s_reports, send_report, NULL);
    assert(notified_handle==16 && mbuf.len==6 && !memcmp(mbuf.data,movement,6));
    assert(clicker_reports_busy(&s_reports));
    assert(send_report(movement, NULL));
    assert(notified_handle==16 && mbuf.len==6 && !memcmp(mbuf.data,movement,6));
    s_mouse_subscribed=false; assert(!send_report(movement,NULL)); s_mouse_subscribed=true;
    s_encrypted=false; assert(!send_report(movement,NULL)); s_encrypted=true;
    alloc_fail=true; assert(!send_report(movement,NULL)); alloc_fail=false;
    notify_fail=true; assert(!send_report(movement,NULL)); notify_fail=false;
    clicker_gate_set_active(&s_gate,false); s_host_active=false; s_exit_pending=true;
    assert(!send_report(movement,NULL)); // Old-generation drag cannot escape exit boundary.
    movement[0]=0; assert(!send_report(movement,NULL)); // Unpressed return must also stop.
    assert(send_report(zero,NULL)); // Mouse-up must escape that boundary.
    assert(notified_handle==16 && mbuf.len==6);
    s_connected=false; assert(!send_report(zero,NULL)); s_connected=true;
    clicker_reports_disconnected(&s_reports);
    clicker_gate_set_active(&s_gate,true); clicker_gate_boundary_complete(&s_gate);
    s_report_generation=s_gate.generation; s_host_active=true; s_exit_pending=false;
    assert(clicker_reports_request(&s_reports, CLICKER_TOUCH_NEXT, true));
    uint8_t contact[8]={3,0x88,0x13,0x90,0x1a}, lift[8]={0,0x88,0x13,0x80,0x0c};
    assert(send_report(contact,NULL));
    assert(notified_handle==20 && mbuf.len==5 && !memcmp(mbuf.data,contact,5));
    s_touch_subscribed=false; assert(!send_report(contact,NULL)); s_touch_subscribed=true;
    s_encrypted=false; assert(!send_report(contact,NULL)); s_encrypted=true;
    clicker_gate_set_active(&s_gate,false); s_host_active=false; s_exit_pending=true;
    assert(!send_report(contact,NULL));
    assert(send_report(lift,NULL)); // Nonzero endpoint must not block touch-up on exit.
    assert(notified_handle==20 && mbuf.len==5 && !memcmp(mbuf.data,lift,5));
    clicker_reports_disconnected(&s_reports);
    clicker_gate_set_active(&s_gate,true); clicker_gate_boundary_complete(&s_gate);
    s_report_generation=s_gate.generation; s_host_active=true; s_exit_pending=false;
    s_subscribed=true;
    assert(clicker_reports_request(&s_reports,CLICKER_NEXT,true));
    uint8_t key[8]={0,0,0x4e}; assert(send_report(key,NULL));
    assert(notified_handle==12 && mbuf.len==8 && !memcmp(mbuf.data,key,8));
    unsigned sent_before = notifications;
    s_host_active=false; s_exit_pending=false;
    assert(!send_report(key,NULL) && !send_report(zero,NULL));
    assert(notifications == sent_before); // Ordinary BLE never transmits HID, even key-up.
    s_host_active=true; s_conn_closing=true;
    assert(!send_report(key,NULL) && !send_report(zero,NULL));
    assert(notifications == sent_before); // A successfully terminated session is never ready.
}
'''
class HidTransportTest(unittest.TestCase):
    def test_real_notify_routing_and_release_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'hid.c'; exe=Path(tmp)/'hid'
            c.write_text(BOUNDARY + function(SOURCE.read_text(), 'send_report') + CASES)
            subprocess.run(['cc','-std=c11','-UNDEBUG','-fsanitize=undefined','-I',str(ROOT/'components/core/services/include'), str(c),str(ROOT/'components/core/services/src/clicker_reports.c'),str(ROOT/'components/core/services/src/clicker_gate.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

    def test_report_descriptor_lengths_and_relative_mouse_axes(self):
        raw=SOURCE.read_text().split('s_report_map[] = {',1)[1].split('};',1)[0]
        raw=re.sub(r'/\*.*?\*/','',raw,flags=re.S)
        data=bytes(int(v,16) for v in re.findall(r'0x([0-9a-f]+)',raw))
        state={}; sizes={}; fields=[]; local=[]; pos=0; depth=0
        while pos<len(data):
            prefix=data[pos]; pos+=1
            n=(0,1,2,4)[prefix&3]; value=int.from_bytes(data[pos:pos+n],'little'); pos+=n
            tag=prefix>>4; kind=(prefix>>2)&3
            if kind==1: state[tag]=value
            elif kind==2: local.append((tag,value))
            elif kind==0:
                if tag==10: depth+=1
                if tag==12: depth-=1
                if tag==8:
                    rid=state[8]; bits=state[7]*state[9]; sizes[rid]=sizes.get(rid,0)+bits
                    fields.append((rid,state[0],list(local),state[7],state[9],value))
                local=[]
        self.assertEqual(depth,0)
        self.assertEqual(sizes,{1:64,2:48,3:40})
        self.assertIn((2,1,[(0,0x30),(0,0x31)],16,2,6),fields)
        self.assertIn((2,9,[(1,1),(2,3)],1,3,2),fields)
        self.assertIn((3,13,[(0,0x42),(0,0x32)],1,2,2),fields)
        self.assertIn((3,1,[(0,0x30),(0,0x31)],16,2,2),fields)
        self.assertEqual(state[1],0) # Logical minimum, unsigned absolute axis
        self.assertEqual(state[2],10000)
        self.assertEqual(state[3],0)
        self.assertEqual(state[4],10000)
        source=SOURCE.read_text()
        self.assertIn('s_touch_report_ref[] = {0x03, 0x01}',source)
        self.assertIn('clicker_reports_touch_current(&s_reports), 5',source)
        self.assertIn('s_mouse_report_ref[] = {0x02, 0x01}',source)
        self.assertIn('.val_handle = &s_mouse_report_handle',source)
        self.assertIn('clicker_reports_mouse_current(&s_reports), 6',source)

if __name__=='__main__': unittest.main()
