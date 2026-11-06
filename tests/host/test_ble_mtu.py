"""Run production MTU callbacks with the installed NimBLE MTU request path.

The real SDK exchange, ATT one-shot flag, preferred-MTU update, and negotiated
minimum are extracted unchanged. Only connection storage, mbuf/radio, GATT
procedure storage, and unrelated reset notifications are boundary doubles.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_ble_advertising import function

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
static atomic_bool s_voice_host_ready;
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MYNEWT_VAL(name) 0
#define BLE_ATT_MTU_DFLT 23
#define BLE_ATT_MTU_MAX 527
#define BLE_L2CAP_CID_ATT 4
#define BLE_L2CAP_CHAN_F_TXED_MTU 1
#define BLE_ATT_OP_MTU_REQ 2
#define BLE_GATT_OP_MTU 1
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_DBG_ASSERT assert
#define STATS_INC(...) ((void)0)
#define min(a,b) ((a)<(b)?(a):(b))
#define htole16(n) (n)
static const char *TAG="mtu-test";
static void test_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGI(...) test_log(__VA_ARGS__)
#define ESP_LOGW(...) test_log(__VA_ARGS__)
#define ESP_LOGD(...) test_log(__VA_ARGS__)
struct ble_l2cap_chan { uint16_t flags, my_mtu, peer_mtu; };
struct ble_hs_conn { uint16_t handle; };
struct ble_gap_conn_desc { int unused; };
struct ble_gatt_error { uint16_t status, att_handle; };
typedef int ble_gatt_mtu_fn(uint16_t, const struct ble_gatt_error *, uint16_t, void *);
struct ble_gattc_proc {
    int op; uint16_t conn_handle, cid;
    struct { ble_gatt_mtu_fn *cb; void *cb_arg; } mtu;
};
struct os_mbuf { uint16_t mtu; };
struct ble_att_mtu_cmd { uint16_t bamc_mtu; };
static struct ble_hs_conn link={7};
static struct ble_l2cap_chan channel;
static struct ble_gattc_proc proc;
static struct os_mbuf packet;
static ble_gatt_mtu_fn *pending_callback;
static void *pending_argument;
static bool link_exists=true, allocation_fails, packet_fails, connection_closing;
static unsigned exchange_calls, tx_calls, mtu_reads, gap_lookups;
static uint16_t wire_mtu, ble_att_preferred_mtu_val=256;
static uint16_t s_conn=7;
static bool s_shutting_down, s_adv_mode_change_pending, s_host_synced=true;
static bool s_subscribed, s_capture_subscribed;
static int s_reasm;
static void (*s_disconnect_cb)(void *);
static void *s_disconnect_user;
struct ble_npl_callout { bool due; };
static struct ble_npl_callout s_adv_retry_callout;
static void ble_npl_callout_stop(struct ble_npl_callout *c) { c->due=false; }
static void frame_reasm_reset(int *r) { *r=0; }
static void capture_params_connection_changed_host(void) {}
static void ble_clicker_host_reset(void) { connection_closing=false; }
static bool ble_clicker_connection_closing(uint16_t conn) { (void)conn; return connection_closing; }
static void ble_hs_lock(void) {}
static void ble_hs_unlock(void) {}
static struct ble_hs_conn *ble_hs_conn_find_by_idx(int index) { return !index&&link_exists?&link:NULL; }
static struct ble_l2cap_chan *ble_hs_conn_chan_find_by_scid(struct ble_hs_conn *conn, uint16_t cid) {
    assert(conn==&link && cid==BLE_L2CAP_CID_ATT); return &channel;
}
static int ble_att_conn_chan_find(uint16_t handle, uint16_t cid, struct ble_hs_conn **conn,
                                  struct ble_l2cap_chan **chan) {
    assert(cid==BLE_L2CAP_CID_ATT);
    if(!link_exists || handle!=link.handle) return BLE_HS_ENOTCONN;
    if(conn) *conn=&link;
    if(chan) *chan=&channel;
    return 0;
}
static int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *desc) {
    (void)desc; ++gap_lookups; return link_exists&&handle==link.handle?0:BLE_HS_ENOTCONN;
}
static void *ble_att_cmd_get(uint8_t op, size_t size, struct os_mbuf **out) {
    assert(op==BLE_ATT_OP_MTU_REQ && size==sizeof(struct ble_att_mtu_cmd));
    if(packet_fails) return NULL;
    *out=&packet; return &packet;
}
static int ble_att_tx(uint16_t handle, uint16_t cid, struct os_mbuf *packet) {
    assert(link_exists && handle==link.handle && cid==BLE_L2CAP_CID_ATT);
    ++tx_calls; wire_mtu=packet->mtu; return 0;
}
static struct ble_gattc_proc *ble_gattc_proc_alloc(void) {
    ++exchange_calls; return allocation_fails?NULL:&proc;
}
static void ble_gattc_log_proc_init(const char *message) { (void)message; }
static void ble_gattc_process_status(struct ble_gattc_proc *p, int rc) {
    if(!rc) { assert(p); pending_callback=p->mtu.cb; pending_argument=p->mtu.cb_arg; }
}
static uint16_t ble_att_chan_mtu(const struct ble_l2cap_chan *chan);
static uint16_t ble_att_mtu(uint16_t conn) {
    ++mtu_reads;
    return link_exists&&conn==link.handle?ble_att_chan_mtu(&channel):0;
}
'''

CASES = r'''
static void reset_case(void) {
    memset(&channel,0,sizeof channel); channel.my_mtu=256;
    s_conn=link.handle=7; link_exists=s_host_synced=true;
    allocation_fails=packet_fails=connection_closing=false;
    s_shutting_down=s_adv_mode_change_pending=false;
    exchange_calls=tx_calls=mtu_reads=gap_lookups=0;
    s_mtu_generation=0; ble_att_preferred_mtu_val=256;
    pending_callback=NULL; pending_argument=NULL;
}
static void complete(int status, uint16_t mtu) {
    struct ble_gatt_error error={.status=status}; assert(pending_callback);
    assert(!pending_callback(link.handle,&error,mtu,pending_argument));
}
int main(int argc,char **argv) {
    assert(argc==2); reset_case();
    switch(atoi(argv[1])) {
    case 1: {
        const uint16_t peer[]={23,185,256};
        for(unsigned i=0;i<sizeof peer/sizeof peer[0];++i) {
            reset_case(); start_mtu_negotiation(7);
            assert(exchange_calls==1 && tx_calls==1 && wire_mtu==256);
            assert(channel.flags & BLE_L2CAP_CHAN_F_TXED_MTU);
            channel.peer_mtu=peer[i];
            assert(ble_att_chan_mtu(&channel)==peer[i]); complete(0,peer[i]);
            assert(exchange_calls==1 && tx_calls==1 && ble_att_preferred_mtu_val==256);
        } break;
    }
    case 2:
        start_mtu_negotiation(7);
        // SDK cleanup runs before GAP_DISCONNECT; both handles still look live.
        assert(s_conn==7 && link_exists); complete(BLE_HS_ENOTCONN,0);
        assert(!gap_lookups && !mtu_reads && exchange_calls==1 && tx_calls==1);
        assert(ble_att_preferred_mtu_val==256); break;
    case 3: {
        start_mtu_negotiation(7);
        ble_gatt_mtu_fn *old_callback=pending_callback; void *old_argument=pending_argument;
        on_reset(42); assert(s_conn==BLE_HS_CONN_HANDLE_NONE);
        struct ble_gatt_error error={.status=BLE_HS_ETIMEOUT};
        old_callback(7,&error,0,old_argument);
        assert(!gap_lookups && !mtu_reads && exchange_calls==1);
        // Same numerical handle, new ATT channel and new generation.
        channel=(struct ble_l2cap_chan){.my_mtu=256}; s_conn=7; s_host_synced=true;
        start_mtu_negotiation(7); assert(old_argument!=pending_argument);
        unsigned lookups=gap_lookups,reads=mtu_reads;
        old_callback(7,&error,0,old_argument);
        error.status=0; old_callback(7,&error,185,old_argument);
        assert(gap_lookups==lookups && mtu_reads==reads && exchange_calls==2 && tx_calls==2);
        assert(ble_att_preferred_mtu_val==256); break;
    }
    case 4: {
        const int errors[]={BLE_HS_ETIMEOUT,BLE_HS_EALREADY,BLE_HS_ENOMEM,BLE_HS_ERR_ATT_BASE+6};
        for(unsigned i=0;i<sizeof errors/sizeof errors[0];++i) {
            reset_case(); start_mtu_negotiation(7); complete(errors[i],0);
            assert(exchange_calls==1 && tx_calls==1 && ble_att_preferred_mtu_val==256);
            assert(channel.my_mtu==256 && gap_lookups==1);
        } break;
    }
    case 5:
        start_mtu_negotiation(7); link_exists=false; complete(BLE_HS_ETIMEOUT,0);
        assert(gap_lookups==1 && !mtu_reads && exchange_calls==1 && tx_calls==1);
        assert(ble_att_preferred_mtu_val==256); break;
    case 6:
        for(unsigned state=0;state<5;++state) {
            reset_case();
            s_shutting_down=state==0; s_adv_mode_change_pending=state==1; connection_closing=state==2;
            if(state==3) s_conn=BLE_HS_CONN_HANDLE_NONE;
            if(state==4) s_conn=8;
            start_mtu_negotiation(7); assert(!exchange_calls && !tx_calls);
        } break;
    case 7:
        for(unsigned state=0;state<3;++state) {
            reset_case(); start_mtu_negotiation(7);
            s_shutting_down=state==0; s_adv_mode_change_pending=state==1; connection_closing=state==2;
            complete(BLE_HS_ETIMEOUT,0);
            assert(!gap_lookups && !mtu_reads && exchange_calls==1 && tx_calls==1);
        } break;
    case 8:
        // Direct SDK proof: neither changing preferred nor retrying can renegotiate.
        start_mtu_negotiation(7); channel.peer_mtu=185;
        assert(ble_att_set_preferred_mtu(23)==0 && channel.my_mtu==256);
        assert(ble_gattc_exchange_mtu(7,NULL,NULL)==BLE_HS_EALREADY);
        assert(tx_calls==1 && ble_att_chan_mtu(&channel)==185);
        assert(ble_att_preferred_mtu_val==23); // Why the old global ladder was harmful.
        break;
    case 9:
        for(unsigned stage=0;stage<3;++stage) {
            reset_case(); allocation_fails=stage==0; packet_fails=stage==1; link_exists=stage!=2;
            start_mtu_negotiation(7);
            assert(exchange_calls==1 && !tx_calls && !pending_callback);
            assert(ble_att_preferred_mtu_val==256);
            if(stage==2) assert(!mtu_reads); // Synchronous ENOTCONN is cleanup too.
        } break;
    case 10:
        s_mtu_generation=UINT32_MAX; start_mtu_negotiation(7);
        assert(s_mtu_generation==1 && pending_argument!=NULL);
        complete(0,23); assert(exchange_calls==1); break;
    default: assert(false);
    }
    return 0;
}
'''


class MtuTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        idf = os.environ.get('IDF_PATH')
        description = ROOT / 'build/project_description.json'
        if not idf and description.is_file():
            idf = json.loads(description.read_text())['idf_path']
        if not idf:
            raise RuntimeError('IDF_PATH or build/project_description.json is required')
        host = Path(idf) / 'components/bt/host/nimble/nimble/nimble/host'
        hs = (host / 'include/host/ble_hs.h').read_text()
        errors = '\n'.join(re.search(r'(?m)^#define ' + name + r'\s+[^\n]+', hs).group()
                           for name in ('BLE_HS_ENOTCONN', 'BLE_HS_EALREADY', 'BLE_HS_ETIMEOUT',
                                        'BLE_HS_ENOMEM', 'BLE_HS_EINVAL', 'BLE_HS_ERR_ATT_BASE'))
        att = (host / 'src/ble_att.c').read_text()
        clt = (host / 'src/ble_att_clt.c').read_text()
        gatt = (host / 'src/ble_gattc.c').read_text()
        l2cap = (host / 'src/ble_l2cap.c').read_text()
        sdk = '\n'.join((function(l2cap, 'ble_l2cap_is_mtu_req_sent'), function(att, 'ble_att_chan_mtu'),
                         function(att, 'ble_att_set_preferred_mtu'), function(clt, 'ble_att_clt_tx_mtu'),
                         function(gatt, 'ble_gattc_mtu_tx'), function(gatt, 'ble_gattc_exchange_mtu')))
        source = (ROOT / 'components/platform/platform_esp32/src/ble_config.c').read_text()
        declarations = source[source.index('#define CONFIG_PREFERRED_MTU'):source.index('static bool config_connection_ready')]
        production = '\n'.join(function(source, name) for name in
                               ('config_connection_ready', 'mtu_exch_cb', 'start_mtu_negotiation', 'on_reset'))
        cls.directory = tempfile.TemporaryDirectory(prefix='passport-mtu-')
        directory = Path(cls.directory.name)
        src, cls.exe = directory / 'test.c', directory / 'test'
        src.write_text('\n'.join((errors, BOUNDARY, sdk, declarations, production, CASES)))
        subprocess.run(['cc', '-std=c11', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fno-sanitize-recover=all', str(src), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def case(self, number):
        result = subprocess.run([str(self.exe), str(number)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_peer_23_185_256_uses_one_request_and_negotiated_minimum(self): self.case(1)
    def test_enotconn_before_gap_disconnect_never_downgrades_or_retries(self): self.case(2)
    def test_host_reset_and_reused_handle_ignore_stale_callbacks(self): self.case(3)
    def test_real_callback_errors_keep_current_mtu_without_retry(self): self.case(4)
    def test_missing_connection_ignores_non_cleanup_callback(self): self.case(5)
    def test_invalid_or_closing_connections_do_not_start_exchange(self): self.case(6)
    def test_pending_shutdown_mode_switch_and_termination_ignore_callback(self): self.case(7)
    def test_real_sdk_locks_mtu_after_first_request(self): self.case(8)
    def test_synchronous_allocation_and_disconnection_errors_do_not_loop(self): self.case(9)
    def test_generation_wrap_skips_null_callback_tag(self): self.case(10)
