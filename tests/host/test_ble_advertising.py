"""Run production advertising/callback code with ESP-IDF's real AD serializer.

Only the HCI/radio, event queue and unrelated app callbacks are substituted.
This catches lost restarts and false pairing status, and checks the exact bytes
handed to the controller. It does not claim reception by a physical computer.
Set IDF_PATH, or build once so build/project_description.json locates the SDK.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"(?m)^(?:static\s+)?(?:const\s+)?\w+\s+\*?" + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(f"Missing production function {name}")
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[match.end() - 1:]):
        if token.group() == "{": depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[match.start():match.end() + token.end() - 1]
    raise AssertionError(f"Unclosed production function {name}")


BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
static atomic_bool s_voice_host_ready;
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform/platform_clicker.h"
#define ESP_MAC_WIFI_STA 0
#define ESP_OK 0
static uint8_t test_mac[6] = {0x4c,0x11,0xae,0x32,0xd7,0xdc};
static int mac_error;
static int esp_read_mac(uint8_t *out, int type) {
    assert(type==ESP_MAC_WIFI_STA);
    if(mac_error) return mac_error;
    memcpy(out,test_mac,6); return 0;
}
static platform_clicker_snapshot_t s_snapshot;
#define BLE_HCI_MAX_ADV_DATA_LEN 31
#define NIMBLE_BLE_ADVERTISE 1
#define BLE_HS_DBG_ASSERT assert
#define VERIFY_UUID(uuid) ((void)0)
#define BLE_HS_EMSGSIZE 4
#define BLE_HS_EINVAL 3
#define BLE_HS_ENOTSUP 8
#define BLE_HS_EALREADY 2
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_HS_FOREVER 0x7fffffff
#define BLE_GAP_CONN_MODE_UND 1
#define BLE_GAP_DISC_MODE_GEN 2
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#include "host/ble_hs_adv.h"
static uint8_t ble_uuid_base[16];
static void put_le16(void *p, uint16_t n) { uint8_t *b=p; b[0]=n; b[1]=n>>8; }
static void put_le32(void *p, uint32_t n) { put_le16(p,n); put_le16((uint8_t *)p+2,n>>16); }
#define htole16(n) (n)
#define htole32(n) (n)
struct os_mbuf { int unused; };
int os_mbuf_append(struct os_mbuf *om, const void *p, uint16_t n) { abort(); }
int ble_uuid_to_mbuf(const ble_uuid_t *u, struct os_mbuf *om) { abort(); }
int ble_hs_hci_util_read_adv_tx_pwr(int8_t *p) { abort(); }

enum { BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_CONN_UPDATE,
       BLE_GAP_EVENT_SUBSCRIBE, BLE_GAP_EVENT_MTU, BLE_GAP_EVENT_ADV_COMPLETE,
       BLE_GAP_EVENT_REPEAT_PAIRING };
struct ble_gap_event {
    int type;
    struct { int status; uint16_t conn_handle; } connect;
    struct { struct { uint16_t conn_handle; } conn; int reason; } disconnect;
    struct { int status; uint16_t conn_handle; } conn_update;
    struct { uint16_t attr_handle; bool cur_notify, cur_indicate; } subscribe;
    struct { uint16_t value; } mtu;
};
struct ble_gap_conn_desc { uint16_t conn_itvl, conn_latency, supervision_timeout; };
struct ble_gap_adv_params { uint8_t conn_mode, disc_mode; };
struct ble_gap_upd_params { uint16_t itvl_min, itvl_max, latency, supervision_timeout; };
struct ble_npl_event { bool queued; void (*cb)(struct ble_npl_event *); };
struct ble_npl_callout { struct ble_npl_event event; bool due; };
static bool s_shutting_down, s_host_synced = true, s_subscribed, s_capture_subscribed;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE, s_notify_handle=10, s_capture_handle=11;
static uint8_t s_addr_type;
static char s_dev_name[32] = "PASSPORT-TEST";
static struct ble_npl_event s_adv_restart_event;
static struct ble_npl_callout s_adv_retry_callout;
static bool s_adv_mode_change_pending;
static uint32_t s_mtu_generation;
static void (*s_conn_cb)(void *), (*s_disconnect_cb)(void *);
static void *s_conn_user, *s_disconnect_user;
static int s_reasm;
static bool clicker_mode, radio_active, reported_active, pairing_paused;
static bool connection_closing;
static int field_error, start_error, stop_error, terminate_error, address_error, reported_error;
static int starts, stops, terminated, status_reports, scans, scan_error, prune_error;
static bool observer_active;
static int disconnects;
static int parameter_updates, mtu_starts, connected_callbacks;
static void connected(void *user) { (void)user; ++connected_callbacks; }
static void disconnected(void *user) { ++disconnects; }
static uint8_t payload[31], payload_len;
static char gap_name[32];
static uint16_t gap_appearance;
static bool ble_clicker_advertising_active(void) { return clicker_mode; }
static bool ble_clicker_pairing_paused_host(void) { return pairing_paused; }
static int ble_clicker_prepare_services_host(bool active) { (void)active; return 0; }
static bool ble_clicker_connection_closing(uint16_t conn) { (void)conn; return connection_closing; }
const char *ble_clicker_name(void);
static void ble_clicker_advertising_result_host(bool active, int error) {
    reported_active=active; reported_error=error; ++status_reports;
}
static void ble_clicker_host_reset(void) { reported_active=false; }
static void ble_clicker_host_synced(void) {}
static int ble_clicker_gap_event(struct ble_gap_event *e) { return 0; }
static void frame_reasm_reset(int *p) { *p=0; }
static int ble_gap_conn_find(uint16_t c, struct ble_gap_conn_desc *d) { return 1; }
static int ble_att_mtu(uint16_t c) { return 23; }
static int ble_gap_update_params(uint16_t c, const struct ble_gap_upd_params *p) { ++parameter_updates; return 0; }
static void start_mtu_negotiation(uint16_t c) { ++mtu_starts; }
static int ble_hs_id_infer_auto(int privacy, uint8_t *type) { *type=0; return address_error; }
static int ble_scan_set_enabled(uint8_t type, bool enabled) {
    if(scan_error)return scan_error;
    observer_active=enabled; ++scans; return 0;
}
static int ble_clicker_prune_orphan_subscriptions(void) {
    assert(!observer_active && !radio_active && s_conn==BLE_HS_CONN_HANDLE_NONE);
    return prune_error;
}
static int ble_svc_gap_device_name_set(const char *name) { strcpy(gap_name,name); return 0; }
static int ble_svc_gap_device_appearance_set(uint16_t n) { gap_appearance=n; return 0; }
static int ble_gap_adv_active(void) { return radio_active; }
static int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) {
    if(field_error) return field_error;
    return ble_hs_adv_set_fields(f,payload,&payload_len,sizeof payload);
}
static int ble_gap_adv_start(uint8_t type, const void *peer, int duration,
        const struct ble_gap_adv_params *p, int (*cb)(struct ble_gap_event *,void *), void *arg) {
    assert(!radio_active && !peer && p->conn_mode==BLE_GAP_CONN_MODE_UND);
    assert(observer_active == !clicker_mode);
    assert(p->disc_mode==BLE_GAP_DISC_MODE_GEN && duration==BLE_HS_FOREVER);
    ++starts; if(start_error) return start_error; radio_active=true; return 0;
}
static int ble_gap_adv_stop(void) {
    ++stops; if(stop_error) return stop_error; radio_active=false; return 0;
}
static int ble_gap_terminate(uint16_t c, int reason) {
    assert(c==7 && reason==BLE_ERR_REM_USER_CONN_TERM); ++terminated; return terminate_error;
}
static bool ble_npl_event_is_queued(struct ble_npl_event *e) { return e->queued; }
static void *nimble_port_get_dflt_eventq(void) { return NULL; }
static void ble_npl_eventq_put(void *q, struct ble_npl_event *e) { e->queued=true; }
static unsigned ble_npl_time_ms_to_ticks32(unsigned ms) { return ms; }
static void ble_npl_callout_stop(struct ble_npl_callout *c) { c->due=false; }
static int ble_npl_callout_reset(struct ble_npl_callout *c, unsigned ticks) {
    assert(ticks>=1000); c->due=true; return 0;
}
// Capture policy is independently exercised by test_ble_capture_transport.py.
static void capture_params_connection_changed_host(void) {}
static void capture_params_updated_host(uint16_t conn,int status) {(void)conn;(void)status;}
static int gap_event(struct ble_gap_event *,void *);
static void queue_adv_restart(void);
void ble_config_clicker_mode_changed_host(void);
'''

CASES = r'''
static void drain(void) {
    assert(s_adv_restart_event.queued);
    s_adv_restart_event.queued=false; adv_restart_cb(&s_adv_restart_event);
}
static void retry(void) {
    assert(s_adv_retry_callout.due);
    s_adv_retry_callout.due=false; adv_restart_cb(&s_adv_retry_callout.event);
}
static void check_clicker_payload(void) {
    static const uint8_t expected[] = {
        2,1,6, 3,3,0x12,0x18, 19,9,
        'A','I',' ','P','A','S','S','P','O','R','T',' ','3','2','D','7','D','C',
        3,0x19,0xc0,3
    };
    assert(payload_len==sizeof expected && !memcmp(payload,expected,sizeof expected));
    assert(!strcmp(gap_name,"AI PASSPORT 32D7DC") && gap_appearance==0x03c0);
}
int main(int argc, char **argv) {
    assert(argc==2); clicker_mode=true;
    assert(clicker_init_name()==0);
    assert(!strcmp(s_snapshot.name,"AI PASSPORT 32D7DC"));
    switch(atoi(argv[1])) {
    case 1:
        start_adv(); assert(radio_active); check_clicker_payload(); break;
    case 2: {
        struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT,.connect={0x3e,7}};
        gap_event(&e,NULL); drain(); assert(radio_active); break;
    }
    case 3:
        field_error=4; start_adv(); assert(!radio_active && starts==0);
        assert(reported_error==4 && !reported_active); break;
    case 4:
        start_error=42; start_adv(); assert(!radio_active);
        assert(reported_error==42 && !reported_active); break;
    case 5:
        radio_active=true; stop_error=42; ble_config_clicker_mode_changed_host();
        assert(reported_error==42 && !reported_active && !s_adv_restart_event.queued); break;
    case 6:
        s_conn=7; terminate_error=42; ble_config_clicker_mode_changed_host();
        assert(terminated==1 && reported_error==42 && !reported_active); break;
    case 7:
        s_conn=7; s_subscribed=s_capture_subscribed=true;
        on_reset(42); on_sync();
        assert(s_conn==BLE_HS_CONN_HANDLE_NONE && !s_subscribed && !s_capture_subscribed);
        assert(radio_active); break;
    case 8:
        clicker_mode=false; start_adv(); assert(radio_active);
        clicker_mode=true; ble_config_clicker_mode_changed_host();
        assert(!radio_active && starts==1); drain();
        check_clicker_payload(); assert(reported_active && reported_error==0); break;
    case 9:
        start_adv(); clicker_mode=false; ble_config_clicker_mode_changed_host(); drain();
        assert(!strcmp(gap_name,"PASSPORT-TEST") && gap_appearance==0);
        assert(radio_active && observer_active && payload_len==18 && payload[4]==9); break;
    case 10:
        s_conn=7; ble_config_clicker_mode_changed_host();
        assert(terminated==1 && !s_adv_restart_event.queued);
        struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={{7},0x13}};
        gap_event(&e,NULL); drain(); assert(radio_active); check_clicker_payload(); break;
    case 11:
        field_error=4; start_adv(); field_error=0; retry(); assert(radio_active); break;
    case 12:
        start_error=42; start_adv(); start_error=0; retry(); assert(radio_active); break;
    case 13:
        radio_active=true; stop_error=42; ble_config_clicker_mode_changed_host();
        stop_error=0; retry(); drain(); assert(radio_active); check_clicker_payload(); break;
    case 14:
        s_conn=7; terminate_error=42; ble_config_clicker_mode_changed_host();
        terminate_error=0; retry(); assert(terminated==2);
        e=(struct ble_gap_event){.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={{7},0x13}};
        gap_event(&e,NULL); drain(); assert(radio_active); break;
    case 15:
        s_disconnect_cb=disconnected; s_conn=7; on_reset(42);
        assert(disconnects==1); on_reset(42); assert(disconnects==1); break;
    case 16:
        start_error=42; start_adv(); on_reset(42);
        assert(!s_adv_retry_callout.due);
        adv_restart_cb(&s_adv_restart_event); assert(starts==1); break;
    case 17:
        start_error=42; start_adv(); s_shutting_down=true;
        retry(); assert(starts==1 && !radio_active); break;
    case 18:
        pairing_paused=true; start_adv(); assert(!radio_active && starts==0);
        pairing_paused=false; ble_config_clicker_mode_changed_host(); drain();
        assert(radio_active); check_clicker_payload(); break;
    case 19:
        s_conn=7; pairing_paused=true;
        e=(struct ble_gap_event){.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={{7},0x13}};
        gap_event(&e,NULL); drain(); assert(!radio_active && starts==0);
        pairing_paused=false; ble_config_clicker_mode_changed_host(); drain();
        assert(radio_active); check_clicker_payload(); break;
    case 20:
        observer_active=true; scan_error=15; start_adv();
        assert(!radio_active && !starts && reported_error==15);
        scan_error=0; retry(); assert(radio_active && !observer_active); break;
    case 21:
        prune_error=30; start_adv(); assert(!radio_active && !starts && reported_error==30);
        prune_error=0; retry(); assert(radio_active && !observer_active); break;
    case 22:
        test_mac[4]=0xe7; test_mac[5]=0x64;
        assert(clicker_init_name()==0);
        assert(!strcmp(ble_clicker_name(),"AI PASSPORT 32E764"));
        start_adv(); assert(radio_active && payload_len==31);
        assert(!strcmp(gap_name,s_snapshot.name));
        assert(clicker_init_name()==0);
        assert(!strcmp(ble_clicker_name(),"AI PASSPORT 32E764")); break;
    case 23:
        mac_error=12; assert(clicker_init_name()==BLE_HS_EINVAL); break;
    case 24:
        test_mac[3]=0; test_mac[4]=1; test_mac[5]=0x0a;
        assert(clicker_init_name()==0);
        assert(!strcmp(ble_clicker_name(),"AI PASSPORT 00010A"));
        start_adv(); assert(radio_active && payload_len==31); break;
    case 25:
    case 26:
    case 27: {
        int mode=atoi(argv[1]);
        connection_closing=mode==25; s_shutting_down=mode==26; s_adv_mode_change_pending=mode==27;
        s_conn_cb=connected;
        struct ble_gap_event event={.type=BLE_GAP_EVENT_CONNECT,.connect={0,7}};
        gap_event(&event,NULL);
        assert(s_conn==7 && s_mtu_generation!=0);
        assert(!parameter_updates && !mtu_starts && !connected_callbacks);
        break;
    }
    case 28: {
        s_conn_cb=connected;
        struct ble_gap_event event={.type=BLE_GAP_EVENT_CONNECT,.connect={0,7}};
        gap_event(&event,NULL);
        assert(s_conn==7 && parameter_updates==1 && mtu_starts==1 && connected_callbacks==1);
        uint32_t generation=s_mtu_generation;
        event=(struct ble_gap_event){.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={{7},0x13}};
        gap_event(&event,NULL);
        assert(s_conn==BLE_HS_CONN_HANDLE_NONE && generation!=s_mtu_generation);
        break;
    }
    case 29: {
        s_conn=8; connection_closing=true;
        struct ble_gap_event event={.type=BLE_GAP_EVENT_CONNECT,.connect={0,7}};
        gap_event(&event,NULL);
        assert(s_conn==8 && !terminated && !parameter_updates && !mtu_starts && !connected_callbacks);
        break;
    }
    default: assert(false);
    }
}
'''


class AdvertisingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get("IDF_PATH")
        description = ROOT / "build/project_description.json"
        if not idf and description.exists():
            idf = json.loads(description.read_text())["idf_path"]
        if not idf:
            raise RuntimeError("IDF_PATH or build/project_description.json is required for the real NimBLE serializer")
        host = Path(idf) / "components/bt/host/nimble/nimble/nimble/host"
        sdk_adv = (host / "src/ble_hs_adv.c").read_text()
        sdk_uuid = (host / "src/ble_uuid.c").read_text()
        production = (ROOT / "components/platform/platform_esp32/src/ble_config.c").read_text()
        clicker = (ROOT / "components/platform/platform_esp32/src/platform_clicker.c").read_text()
        cls.directory = tempfile.TemporaryDirectory(prefix="passport-adv-")
        directory = Path(cls.directory.name)
        (directory / "syscfg").mkdir()
        (directory / "syscfg/syscfg.h").write_text("#define MYNEWT_VAL(name) 0\n")
        name = re.search(r'(?m)^#define CLICKER_NAME .*$', clicker).group()
        code = BOUNDARY + name + "\n" + function(clicker,"clicker_init_name") + function(clicker,"ble_clicker_name") + function(sdk_uuid,"ble_uuid_flat") + "\n".join(function(sdk_adv,name) for name in (
            "ble_hs_adv_set_hdr", "ble_hs_adv_set_flat_mbuf", "ble_hs_adv_set_array_uuid16",
            "ble_hs_adv_set_array_uuid32", "ble_hs_adv_set_array_uuid128", "ble_hs_adv_set_array16",
            "adv_set_fields", "ble_hs_adv_set_fields")) + "\n".join(function(production,name) for name in (
            "config_connection_ready", "schedule_adv_retry", "start_adv", "adv_restart_cb", "queue_adv_restart", "ble_config_clicker_mode_changed_host",
            "gap_event", "on_sync", "on_reset")) + CASES
        (directory / "advertising.c").write_text(code)
        cls.executable = directory / "advertising"
        subprocess.run(["cc", "-std=c11", "-UNDEBUG", "-I", str(directory), "-I", str(host / "include"),
                        "-I", str(ROOT / "components/platform/platform_esp32/include"),
                        "-I", str(ROOT / "components/core/services/include"),
                        str(directory / "advertising.c"), "-o", str(cls.executable)], check=True)

    @classmethod
    def tearDownClass(cls): cls.directory.cleanup()

    def case(self, n):
        result = subprocess.run([str(self.executable), str(n)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_hid_discovery_bytes_fit_legacy_advertisement(self): self.case(1)
    def test_second_device_name_is_distinct_stable_and_matches_snapshot(self): self.case(22)
    def test_hardware_identity_error_is_not_advertised_as_a_generic_name(self): self.case(23)
    def test_suffix_retains_leading_zeroes_and_uppercase(self): self.case(24)
    def test_failed_connection_restarts_advertising(self): self.case(2)
    def test_field_failure_is_reported_to_clicker(self): self.case(3)
    def test_failed_scan_pause_blocks_pairing_and_retries(self): self.case(20)
    def test_failed_orphan_cleanup_blocks_pairing_and_retries(self): self.case(21)
    def test_start_failure_is_reported_to_clicker(self): self.case(4)
    def test_failed_stop_cannot_claim_clicker_advertises(self): self.case(5)
    def test_failed_terminate_is_reported_to_clicker(self): self.case(6)
    def test_host_reset_drops_stale_connection_before_resync(self): self.case(7)
    def test_switch_restarts_on_later_event_with_hid_bytes(self): self.case(8)
    def test_exit_restores_standard_advertisement(self): self.case(9)
    def test_switch_with_connection_waits_for_disconnect(self): self.case(10)
    def test_transient_field_failure_recovers_without_reentry(self): self.case(11)
    def test_transient_start_failure_recovers_without_reentry(self): self.case(12)
    def test_transient_stop_failure_recovers_without_reentry(self): self.case(13)
    def test_transient_disconnect_failure_recovers_without_reentry(self): self.case(14)
    def test_host_reset_notifies_existing_configuration_session_once(self): self.case(15)
    def test_host_reset_cancels_retry_until_resynced(self): self.case(16)
    def test_shutdown_suppresses_pending_advertising_retry(self): self.case(17)
    def test_pending_peer_deletion_pauses_pairing(self): self.case(18)
    def test_disconnect_restart_waits_for_peer_deletion_retry(self): self.case(19)
    def test_closing_connect_has_no_params_mtu_or_business_callback(self): self.case(25)
    def test_shutdown_connect_has_no_params_mtu_or_business_callback(self): self.case(26)
    def test_mode_switch_connect_has_no_params_mtu_or_business_callback(self): self.case(27)
    def test_live_connect_initializes_once_and_disconnect_invalidates_mtu(self): self.case(28)
    def test_already_closing_extra_connect_does_not_terminate_twice(self): self.case(29)


if __name__ == "__main__": unittest.main()
