"""Registration and connected replay run on the same host queue as disconnect."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef void (*cfg_conn_cb_t)(void*);
typedef cfg_conn_cb_t cfg_disconnect_cb_t;
typedef int hal_config_t;
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define portENTER_CRITICAL(p) ((void)p)
#define portEXIT_CRITICAL(p) ((void)p)
static int s_conn_registration_lock;
struct ble_npl_event { bool queued; };
static struct ble_npl_event s_conn_registration_event;
static bool s_conn_registration_ready, s_shutting_down;
static bool s_adv_mode_change_pending, connection_closing;
static bool ble_clicker_connection_closing(uint16_t conn) { (void)conn; return connection_closing; }
static cfg_conn_cb_t s_pending_conn_cb, s_conn_cb, s_disconnect_cb;
static void *s_pending_conn_user, *s_conn_user, *s_disconnect_user;
static uint16_t s_conn=BLE_HS_CONN_HANDLE_NONE;
static void *nimble_port_get_dflt_eventq(void) { return NULL; }
static void ble_npl_eventq_put(void *q, struct ble_npl_event *e) { e->queued=true; }
static unsigned connected, disconnected;
static int token, other;
static void connect_cb(void *p) { assert(p==&token || p==&other); ++connected; }
static void disconnect_cb(void *p) { assert(p==&token); ++disconnected; }
'''
CASES=r'''
static void drain(void) {
    assert(s_conn_registration_event.queued); s_conn_registration_event.queued=false;
    on_conn_registration(&s_conn_registration_event);
}
int main(void) {
    cfg_on_connect(NULL,connect_cb,&token); assert(!s_conn_registration_event.queued);
    s_conn_registration_ready=true;
    cfg_on_disconnect(NULL,disconnect_cb,&token);
    cfg_on_connect(NULL,connect_cb,&token); assert(!s_conn_cb); drain(); assert(!connected);
    s_conn=7; s_conn_cb(s_conn_user); assert(connected==1);
    cfg_on_connect(NULL,connect_cb,&token); drain(); assert(connected==1); // identical no replay
    cfg_on_connect(NULL,connect_cb,&other); drain(); assert(connected==2&&s_conn_user==&other);
    cfg_on_connect(NULL,connect_cb,&token);
    s_conn=BLE_HS_CONN_HANDLE_NONE; s_disconnect_cb(s_disconnect_user); drain();
    assert(connected==2&&disconnected==1); // no stale connected after disconnected
    cfg_on_connect(NULL,NULL,NULL); drain(); assert(!s_conn_cb);
    s_conn=7; cfg_on_connect(NULL,connect_cb,&token); drain(); assert(connected==3); // late registration
    s_shutting_down=true; cfg_on_connect(NULL,connect_cb,&other); drain(); assert(connected==3);
    s_shutting_down=false; connection_closing=true;
    cfg_on_connect(NULL,connect_cb,&token); drain(); assert(connected==3);
    connection_closing=false; s_adv_mode_change_pending=true;
    cfg_on_connect(NULL,connect_cb,&other); drain(); assert(connected==3);
    s_adv_mode_change_pending=false;
    cfg_on_connect(NULL,connect_cb,&token); drain(); assert(connected==4);
    return 0;
}
'''
class CallbackRegistration(unittest.TestCase):
    def test_queue_serialization(self):
        src=(ROOT/'components/platform/platform_esp32/src/ble_config.c').read_text()
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'test.c'; e=Path(d)/'test'
            c.write_text(BOUNDARY+'\n'.join(function(src,n) for n in ['config_connection_ready','on_conn_registration','cfg_on_connect','cfg_on_disconnect'])+CASES)
            subprocess.run(['cc','-std=c11','-fsanitize=undefined','-Wall','-Werror',str(c),'-o',str(e)],check=True)
            subprocess.run([str(e)],check=True)
