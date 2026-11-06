"""Execute the real Clicker GAP callback with a small deterministic radio boundary.

This host harness verifies status derivation and failed-security disconnects;
ESP-IDF compilation and actual computer pairing remain separate checks.
Default sanitizers are address,undefined. PASSPORT_HOST_SANITIZERS=undefined
is an explicit UBSan-only behavior lane for hosts with a broken ASAN runtime.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "components/platform/platform_esp32/src/platform_clicker.c"


def host_sanitizers():
    value = os.environ.get("PASSPORT_HOST_SANITIZERS", "address,undefined")
    if value not in ("address,undefined", "undefined"):
        raise ValueError("PASSPORT_HOST_SANITIZERS must be address,undefined or undefined; "
                         f"got {value!r}; sanitizer selection never falls back automatically")
    return value


def run_host(command, *, timeout, phase, sanitizers, check_main=False, env=None,
             main_marker="PASSPORT_CLICKER_MAIN_ENTERED", case_marker="PASSPORT_CLICKER_CASE "):
    def diagnostic(stdout, stderr):
        stdout = stdout.decode(errors="replace") if isinstance(stdout, bytes) else stdout or ""
        stderr = stderr.decode(errors="replace") if isinstance(stderr, bytes) else stderr or ""
        entered = stdout + stderr
        position = ""
        if check_main:
            if main_marker not in entered:
                position = "main entry not observed; possible sanitizer/loader startup failure"
                if "address" in sanitizers.split(","):
                    position += " (ASAN enabled)"
            elif case_marker not in entered:
                position = "main entered; fixture setup before case body"
            else:
                position = "case body entered; inspect case steps/assertion/sanitizer output"
        return (f"{phase}; sanitizers={sanitizers}; {position}\n"
                f"command={command!r}\nstdout: {stdout}\nstderr: {stderr}")

    try:
        result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"Host {phase} timed out after {timeout}s; "
                             + diagnostic(error.stdout, error.stderr)) from error
    if result.returncode:
        raise AssertionError(f"Host {phase} exited {result.returncode}; "
                             + diagnostic(result.stdout, result.stderr))
    return result


def function(source, name):
    match = re.search(r"(?m)^(?:static )?\w+ " + name + r"\([^;]*?\)\s*\{", source)
    if not match:
        raise AssertionError(f"Missing real function {name}")
    start = match.start()
    depth = 0
    for token in re.finditer(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[{}]', source[match.end() - 1:]):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[start:match.end() + token.end() - 1]
    raise AssertionError(f"Unclosed real function {name}")


BOUNDARY = r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "platform/platform_clicker.h"
#include "services/clicker_gate.h"
#define CLICKER_REPORT_DELAY_MS 25
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_HS_EALREADY 2
#define BLE_HS_ENOTCONN 7
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_GAP_REPEAT_PAIRING_IGNORE 0
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define portENTER_CRITICAL(lock) ((void)0)
#define portEXIT_CRITICAL(lock) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
enum {BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_SUBSCRIBE,
      BLE_GAP_EVENT_ENC_CHANGE, BLE_GAP_EVENT_PASSKEY_ACTION, BLE_GAP_EVENT_REPEAT_PAIRING,
      BLE_GAP_EVENT_NOTIFY_TX};
typedef struct { uint8_t type, val[6]; } ble_addr_t;
struct ble_gap_conn_desc {
    uint16_t conn_handle, conn_itvl, conn_latency;
    ble_addr_t peer_id_addr;
    struct { bool encrypted, bonded; } sec_state;
};
struct ble_gap_event {
    int type;
    struct { int status; uint16_t conn_handle; } connect, enc_change;
    struct { struct ble_gap_conn_desc conn; } disconnect;
    struct { uint16_t conn_handle, attr_handle; bool cur_notify; } subscribe;
    struct { uint16_t conn_handle, attr_handle; int status; } notify_tx;
    struct { uint16_t conn_handle; } passkey, repeat_pairing;
};
static struct { bool sm_bonding; } ble_hs_cfg;
static bool s_initialized = true, s_host_active, s_exit_pending, s_shutting_down;
static bool s_host_synced = true, s_change_requested;
static ble_addr_t s_hid_peer, s_change_peer;
static bool s_hid_peer_valid, s_change_peer_valid, s_change_peer_captured;
static uint32_t s_report_generation;
static int s_report_callout;
struct ble_npl_event { int unused; };
static platform_clicker_snapshot_t s_snapshot;
static int scheduled, last_delay_ms, delete_calls, delete_failures, report_failures, reports_sent, work_scheduled;
static uint8_t last_report[8];
static char trace[128];
static unsigned trace_len;
static void record(char c) { assert(trace_len < sizeof(trace) - 1); trace[trace_len++] = c; }
static int ble_npl_time_ms_to_ticks32(int ms) { return ms; }
static int ble_npl_callout_reset(int *timer, int ms) { (void)timer; assert(ms > 0); ++scheduled; last_delay_ms = ms; return 0; }
static void ble_npl_callout_stop(int *timer) { (void)timer; }
static void schedule_work(void) { ++work_scheduled; }
static void snapshot_sync_counts(void) {}
static bool s_connected, s_encrypted, s_subscribed, s_mouse_subscribed, s_touch_subscribed, s_peer_was_bonded;
static bool s_exit_release_settle_pending, s_advertising;
static int s_advertising_error;
static uint16_t s_conn_handle, s_report_handle = 12, s_mouse_report_handle = 16, s_touch_report_handle = 20;
static bool s_conn_closing;
static uint16_t s_rejected_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static clicker_reports_t s_reports;
static clicker_gate_t s_gate;
static platform_clicker_status_t status;
static int bonds, security_result, security_calls, termination_result, terminated;
static bool allow_live_peer_delete;
static struct ble_gap_conn_desc descriptor;
static void snapshot_set_status(platform_clicker_status_t value) { status = value; s_snapshot.status = value; }
static int ble_store_util_count(int kind, int *count) { (void)kind; *count = bonds; return 0; }
static bool peer_is_bonded(const ble_addr_t *peer) { (void)peer; return bonds > 0; }
static int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out) {
    assert(handle == 7); *out = descriptor; return 0;
}
static int ble_gap_security_initiate(uint16_t handle) { assert(handle == 7); ++security_calls; return security_result; }
static int ble_gap_terminate(uint16_t handle, int reason) {
    assert((handle == 7 || handle == 8) && reason == 0x13); ++terminated; record('T'); return termination_result;
}
static void input_clear(void) { clicker_gate_input_done(&s_gate); }
static bool exit_latched(void) { return s_gate.exit_latched; }
static int ble_store_util_delete_peer(const ble_addr_t *peer) {
    assert(s_conn_handle == BLE_HS_CONN_HANDLE_NONE || allow_live_peer_delete);
    assert(peer->type == 1 && peer->val[0] == 0xa7); /* Only this HID peer. */
    ++delete_calls; record('B');
    if (delete_failures) { --delete_failures; return 42; }
    return 0;
}
static bool send_report(const uint8_t data[8], void *arg) {
    (void)arg;
    if (report_failures) { --report_failures; record('f'); return false; }
    memcpy(last_report, data, 8); ++reports_sent; record(data[2] ? 'K' : 'R'); return true;
}
void ble_clicker_advertising_result_host(bool active, int error);
static void ble_config_clicker_mode_changed_host(void) {
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        record('A'); ble_clicker_advertising_result_host(s_host_active, 0);
    }
}
'''

CASES = r'''
static void reset(void) {
    clicker_gate_init(&s_gate); clicker_gate_set_active(&s_gate, true);
    s_host_active = true; s_exit_pending = s_shutting_down = false;
    s_connected = s_encrypted = s_subscribed = s_mouse_subscribed = s_touch_subscribed = s_peer_was_bonded = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_advertising = true; s_advertising_error = 0;
    bonds = security_result = security_calls = termination_result = terminated = 0;
    s_conn_closing = allow_live_peer_delete = false;
    s_rejected_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_change_requested = s_hid_peer_valid = s_change_peer_valid = s_change_peer_captured = false;
    s_exit_release_settle_pending = false;
    scheduled = delete_calls = delete_failures = report_failures = reports_sent = work_scheduled = 0;
    trace_len = 0; memset(trace, 0, sizeof trace);
    clicker_reports_init(&s_reports);
    memset(&descriptor, 0, sizeof descriptor); descriptor.conn_handle = 7;
    descriptor.peer_id_addr.type = 1; descriptor.peer_id_addr.val[0] = 0xa7;
}
static void ready(void) {
    bonds = 2; /* Another saved peer must never be targeted. */
    descriptor.sec_state.encrypted = descriptor.sec_state.bonded = true;
    struct ble_gap_event event = {.type = BLE_GAP_EVENT_CONNECT, .connect = {0, 7}};
    ble_clicker_gap_event(&event);
    event.type = BLE_GAP_EVENT_SUBSCRIBE;
    event.subscribe.conn_handle = 7; event.subscribe.attr_handle = 12; event.subscribe.cur_notify = true;
    ble_clicker_gap_event(&event); assert(status == PLATFORM_CLICKER_READY);
    assert(s_hid_peer_valid);
}
static void disconnected(void) {
    record('D');
    struct ble_gap_event event = {.type = BLE_GAP_EVENT_DISCONNECT};
    event.disconnect.conn = descriptor;
    ble_clicker_gap_event(&event);
    if (!ble_clicker_pairing_paused_host()) {
        record('A'); ble_clicker_advertising_result_host(s_host_active, 0);
    }
}
int main(int argc, char **argv) {
    fputs("PASSPORT_CLICKER_MAIN_ENTERED\n", stderr); fflush(stderr);
    assert(argc == 2); reset();
    fprintf(stderr, "PASSPORT_CLICKER_CASE %s\n", argv[1]); fflush(stderr);
    struct ble_gap_event event = {.type = BLE_GAP_EVENT_CONNECT, .connect = {0, 7}};
    switch (atoi(argv[1])) {
    case 1:
        bonds = 1; refresh_ready_status();
        assert(status == PLATFORM_CLICKER_PAIRING);
        break;
    case 2:
        security_result = 42; ble_clicker_gap_event(&event);
        assert(terminated == 1 && ble_clicker_connection_closing(7));
        break;
    case 3:
        ble_clicker_gap_event(&event);
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 42; ble_clicker_gap_event(&event);
        assert(terminated == 1 && status != PLATFORM_CLICKER_READY);
        break;
    case 4:
        security_result = BLE_HS_EALREADY; ble_clicker_gap_event(&event);
        assert(terminated == 0 && status != PLATFORM_CLICKER_READY);
        descriptor.sec_state.encrypted = true;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 0; ble_clicker_gap_event(&event);
        assert(status != PLATFORM_CLICKER_READY);
        event.type = BLE_GAP_EVENT_SUBSCRIBE; event.subscribe.conn_handle = 7;
        event.subscribe.attr_handle = 12; event.subscribe.cur_notify = true;
        ble_clicker_gap_event(&event); assert(status == PLATFORM_CLICKER_READY);
        event.type = BLE_GAP_EVENT_DISCONNECT; event.disconnect.conn.conn_handle = 7;
        ble_clicker_gap_event(&event); assert(status == PLATFORM_CLICKER_CONNECTING);
        ble_clicker_advertising_result_host(true, 0);
        assert(status == PLATFORM_CLICKER_PAIRING);
        break;
    case 5:
        ble_clicker_advertising_result_host(false, 0);
        assert(status == PLATFORM_CLICKER_CONNECTING);
        break;
    case 6:
        ble_clicker_advertising_result_host(false, 42);
        assert(status == PLATFORM_CLICKER_UNAVAILABLE);
        ble_clicker_advertising_result_host(true, 0);
        assert(status == PLATFORM_CLICKER_PAIRING);
        break;
    case 7: /* A held key must be released before termination and peer deletion. */
        ready(); assert(platform_clicker_send(CLICKER_NEXT)); work_event_cb(NULL);
        assert(last_report[2] == 0x4e);
        assert(platform_clicker_change_computer());
        assert(!platform_clicker_send(CLICKER_NEXT));
        assert(!platform_clicker_change_computer());
        work_event_cb(NULL); assert(terminated == 0 && delete_calls == 0);
        report_failures = 1; process_report_tick(); assert(terminated == 0);
        descriptor.conn_itvl = 40; descriptor.conn_latency = 1;
        process_report_tick(); assert(last_report[2] == 0 && terminated == 0);
        assert(last_delay_ms >= 105); /* 50 ms interval, one slave-latency event. */
        process_report_tick(); assert(terminated == 1 && delete_calls == 0);
        disconnected(); assert(strcmp(trace, "KfRTDBA") == 0);
        assert(status == PLATFORM_CLICKER_PAIRING && delete_calls == 1);
        break;
    case 8: /* Queued old-session input is cancelled before host work runs. */
        ready(); assert(platform_clicker_send(CLICKER_NEXT));
        assert(platform_clicker_change_computer()); work_event_cb(NULL);
        assert(reports_sent == 0 && terminated == 1);
        disconnected(); assert(status == PLATFORM_CLICKER_PAIRING);
        break;
    case 9: /* GAP disconnect wins the race against the marshalled request. */
        ready(); assert(platform_clicker_change_computer()); disconnected();
        assert(delete_calls == 1 && status == PLATFORM_CLICKER_PAIRING);
        work_event_cb(NULL); assert(delete_calls == 1 && terminated == 0);
        break;
    case 10: /* A connected unpaired config client is not a HID bond target. */
        ble_clicker_gap_event(&event); assert(!s_hid_peer_valid);
        assert(platform_clicker_change_computer()); work_event_cb(NULL); disconnected();
        assert(delete_calls == 0 && status == PLATFORM_CLICKER_PAIRING);
        break;
    case 11: /* Exit after a switch request wins over the original reenter. */
        ready(); assert(platform_clicker_change_computer());
        platform_clicker_set_active(false); work_event_cb(NULL); disconnected();
        assert(delete_calls == 1 && !s_host_active && status == PLATFORM_CLICKER_OFF);
        break;
    case 12: /* Failed peer writes keep advertising paused and retry same peer. */
        ready(); assert(platform_clicker_change_computer()); work_event_cb(NULL);
        delete_failures = 1; disconnected();
        assert(delete_calls == 1 && ble_clicker_pairing_paused_host());
        assert(status == PLATFORM_CLICKER_UNAVAILABLE && !strchr(trace, 'A'));
        process_report_tick(); process_report_tick();
        assert(delete_calls == 2 && !ble_clicker_pairing_paused_host());
        assert(status == PLATFORM_CLICKER_PAIRING);
        break;
    case 13: /* Extra/late peer events cannot replace the captured HID identity. */
        ready(); event.connect.conn_handle = 8; ble_clicker_gap_event(&event);
        assert(s_conn_handle == 7 && terminated == 1);
        event.type = BLE_GAP_EVENT_DISCONNECT; event.disconnect.conn.conn_handle = 8;
        ble_clicker_gap_event(&event); assert(s_conn_handle == 7 && s_hid_peer_valid);
        assert(platform_clicker_change_computer()); disconnected(); assert(delete_calls == 1);
        break;
    case 14: /* Host reset preserves the requested peer deletion across sync. */
        ready(); assert(platform_clicker_change_computer()); ble_clicker_host_reset();
        assert(s_conn_handle == BLE_HS_CONN_HANDLE_NONE && delete_calls == 0);
        ble_clicker_host_synced(); assert(delete_calls == 1 && s_host_active);
        assert(!ble_clicker_pairing_paused_host());
        break;
    case 15: /* Exhausted key-up retries still request a managed disconnect. */
        ready(); assert(platform_clicker_send(CLICKER_NEXT)); work_event_cb(NULL);
        assert(platform_clicker_change_computer()); work_event_cb(NULL);
        report_failures = 3;
        process_report_tick(); process_report_tick(); process_report_tick();
        assert(s_reports.disconnect_required && terminated == 1 && delete_calls == 0);
        disconnected(); assert(delete_calls == 1 && !clicker_reports_busy(&s_reports));
        break;
    case 16: /* No live HID computer means restart pairing without deleting bonds. */
        bonds = 2; assert(platform_clicker_change_computer()); work_event_cb(NULL);
        assert(delete_calls == 0 && terminated == 0 && status == PLATFORM_CLICKER_PAIRING);
        break;
    case 17: /* Late encryption cannot bypass an outstanding key release. */
        ready(); assert(platform_clicker_send(CLICKER_NEXT)); work_event_cb(NULL);
        assert(platform_clicker_change_computer());
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 0; ble_clicker_gap_event(&event);
        assert(terminated == 0 && delete_calls == 0 && s_exit_pending);
        process_report_tick(); process_report_tick();
        assert(terminated == 1 && last_report[2] == 0);
        disconnected(); assert(strcmp(trace, "KRTDBA") == 0);
        break;
    case 18: /* A bond completing during switch is deleted only after GAP drop. */
        ble_clicker_gap_event(&event);
        assert(platform_clicker_change_computer()); work_event_cb(NULL);
        assert(terminated == 1 && delete_calls == 0);
        descriptor.sec_state.encrypted = descriptor.sec_state.bonded = true;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 0; ble_clicker_gap_event(&event);
        assert(delete_calls == 0);
        disconnected(); assert(delete_calls == 1);
        break;
    case 19: // Mouse-only iPhone: drag release precedes peer switch/disconnect.
        ble_clicker_gap_event(&event);
        descriptor.sec_state.encrypted = descriptor.sec_state.bonded = true;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        ble_clicker_gap_event(&event);
        event.type = BLE_GAP_EVENT_SUBSCRIBE; event.subscribe.conn_handle = 7;
        event.subscribe.attr_handle = 16; event.subscribe.cur_notify = true;
        ble_clicker_gap_event(&event);
        assert(status == PLATFORM_CLICKER_READY && s_snapshot.mouse_ready && !s_snapshot.keyboard_ready);
        assert(!platform_clicker_send(CLICKER_NEXT));
        assert(platform_clicker_send(CLICKER_PHONE_NEXT)); work_event_cb(NULL);
        assert(reports_sent == 1 && last_report[0] == 1);
        for (unsigned i = 0; i < 6; ++i) process_report_tick();
        assert(reports_sent == 7 && !clicker_reports_busy(&s_reports));
        for (unsigned i = 0; i < 6; ++i) assert(clicker_reports_mouse_current(&s_reports)[i] == 0);
        assert(platform_clicker_send(CLICKER_POINTER_TEST)); work_event_cb(NULL);
        const uint8_t probe[8] = {0, 24, 0, 24, 0, 0, 0, 0};
        assert(reports_sent == 8 && !memcmp(last_report, probe, 8));
        assert(platform_clicker_send(CLICKER_PHONE_NEXT)); work_event_cb(NULL);
        assert(last_report[0] == 1);
        assert(platform_clicker_change_computer()); work_event_cb(NULL);
        assert(terminated == 0 && reports_sent == 9);
        process_report_tick(); assert(terminated == 0 && last_report[0] == 0);
        process_report_tick(); assert(terminated == 1 && reports_sent == 10);
        disconnected(); assert(!s_mouse_subscribed && delete_calls == 1);
        break;
    case 20: // A keyboard-only pairing must not claim a phone gesture was sent.
        ready(); assert(!s_snapshot.mouse_ready && s_snapshot.keyboard_ready);
        assert(!platform_clicker_send(CLICKER_PHONE_PREV));
        assert(!platform_clicker_send(CLICKER_POINTER_TEST));
        assert(reports_sent == 0 && !clicker_reports_busy(&s_reports));
        break;
    case 21: // Subscription disappearing between enqueue and host work cancels input.
        ready(); event.type = BLE_GAP_EVENT_SUBSCRIBE; event.subscribe.conn_handle = 7;
        event.subscribe.attr_handle = 16; event.subscribe.cur_notify = true;
        ble_clicker_gap_event(&event); assert(platform_clicker_send(CLICKER_PHONE_NEXT));
        event.subscribe.cur_notify = false; ble_clicker_gap_event(&event);
        work_event_cb(NULL); assert(!clicker_reports_busy(&s_reports) && reports_sent == 0);
        break;
    case 22: // Touch-only Android: no keyboard/mouse fallback, release then disconnect.
        ble_clicker_gap_event(&event);
        descriptor.sec_state.encrypted = descriptor.sec_state.bonded = true;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        ble_clicker_gap_event(&event);
        event.type = BLE_GAP_EVENT_SUBSCRIBE; event.subscribe.conn_handle = 7;
        event.subscribe.attr_handle = 20; event.subscribe.cur_notify = true;
        ble_clicker_gap_event(&event);
        assert(s_snapshot.touch_ready && !s_snapshot.mouse_ready && !s_snapshot.keyboard_ready);
        assert(!platform_clicker_send(CLICKER_PHONE_NEXT));
        assert(!platform_clicker_send(CLICKER_POINTER_TEST));
        assert(!platform_clicker_send(CLICKER_NEXT));
        assert(platform_clicker_send(CLICKER_TOUCH_NEXT)); work_event_cb(NULL);
        assert(clicker_reports_touch_current(&s_reports)[0] == 3);
        assert(platform_clicker_change_computer()); work_event_cb(NULL);
        process_report_tick(); assert(last_report[0] == 0 && terminated == 0);
        process_report_tick(); assert(terminated == 1);
        disconnected(); assert(!s_touch_subscribed && delete_calls == 1);
        break;
    case 23: // A disappeared touch subscription drops queued input.
        ready(); event.type = BLE_GAP_EVENT_SUBSCRIBE; event.subscribe.conn_handle = 7;
        event.subscribe.attr_handle = 20; event.subscribe.cur_notify = true;
        ble_clicker_gap_event(&event); assert(platform_clicker_send(CLICKER_TOUCH_NEXT));
        event.subscribe.cur_notify = false; ble_clicker_gap_event(&event);
        work_event_cb(NULL); assert(!clicker_reports_busy(&s_reports) && reports_sent == 0);
        break;
    case 24: // A saved phone reconnecting in standard mode is not a HID session.
        clicker_gate_set_active(&s_gate, false); clicker_gate_boundary_complete(&s_gate);
        s_host_active = false; bonds = 1;
        ble_clicker_gap_event(&event);
        assert(terminated == 0 && s_connected && s_conn_handle == 7);
        assert(!ble_clicker_connection_closing(7) && security_calls == 0);
        descriptor.sec_state.encrypted = descriptor.sec_state.bonded = true;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 0; ble_clicker_gap_event(&event);
        assert(terminated == 0 && delete_calls == 0 && s_encrypted);
        event.type = BLE_GAP_EVENT_SUBSCRIBE; event.subscribe.conn_handle = 7;
        event.subscribe.cur_notify = true;
        const uint16_t handles[] = {12, 16, 20};
        for (unsigned i = 0; i < 3; ++i) {
            event.subscribe.attr_handle = handles[i]; ble_clicker_gap_event(&event);
        }
        assert(status == PLATFORM_CLICKER_OFF && !s_hid_peer_valid);
        assert(!s_snapshot.keyboard_ready && !s_snapshot.mouse_ready && !s_snapshot.touch_ready);
        assert(!platform_clicker_send(CLICKER_NEXT));
        assert(!platform_clicker_send(CLICKER_PHONE_NEXT));
        assert(!platform_clicker_send(CLICKER_TOUCH_NEXT));
        event.type = BLE_GAP_EVENT_REPEAT_PAIRING; event.repeat_pairing.conn_handle = 7;
        assert(ble_clicker_gap_event(&event) == BLE_GAP_REPEAT_PAIRING_IGNORE);
        assert(!reports_sent && !delete_calls && !terminated && !ble_clicker_connection_closing(7));
        assert(!ble_hs_cfg.sm_bonding && security_calls == 0);
        break;
    case 25: // ENOTCONN is teardown notification, not a new encryption failure.
        ble_clicker_gap_event(&event);
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = BLE_HS_ENOTCONN; ble_clicker_gap_event(&event);
        assert(terminated == 0 && !s_encrypted && status != PLATFORM_CLICKER_READY);
        assert(ble_clicker_connection_closing(7));
        event.enc_change.status = 42; ble_clicker_gap_event(&event);
        assert(terminated == 0); // No second request after teardown is known.
        break;
    case 26: // Failed termination remains retryable; accepted/EALREADY is latched.
        termination_result = 42; security_result = 42;
        ble_clicker_gap_event(&event);
        assert(terminated == 1 && !ble_clicker_connection_closing(7));
        termination_result = BLE_HS_EALREADY;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 42; ble_clicker_gap_event(&event);
        assert(terminated == 2 && ble_clicker_connection_closing(7));
        ble_clicker_gap_event(&event); assert(terminated == 2);
        disconnected(); assert(!ble_clicker_connection_closing(7));
        event.type = BLE_GAP_EVENT_CONNECT; event.connect.status = 0; event.connect.conn_handle = 7;
        security_result = 0; ble_clicker_gap_event(&event);
        assert(!ble_clicker_connection_closing(7)); // Reused handle is a new session.
        break;
    case 27: // Rejecting an extra connection never marks the primary as closing.
        ready(); event.connect.conn_handle = 8; ble_clicker_gap_event(&event);
        assert(ble_clicker_connection_closing(8) && !ble_clicker_connection_closing(7));
        assert(s_conn_handle == 7 && s_hid_peer_valid);
        ble_clicker_gap_event(&event); assert(terminated == 1);
        event.type = BLE_GAP_EVENT_DISCONNECT; event.disconnect.conn.conn_handle = 8;
        ble_clicker_gap_event(&event);
        assert(!ble_clicker_connection_closing(8) && !ble_clicker_connection_closing(7));
        assert(request_disconnect(7) == 0 && ble_clicker_connection_closing(7));
        ble_clicker_host_reset();
        assert(!ble_clicker_connection_closing(7) && !ble_clicker_connection_closing(8));
        assert(!ble_clicker_connection_closing(BLE_HS_CONN_HANDLE_NONE));
        break;
    case 28: // Standard mode does not retain an unexpected newly created bond.
        clicker_gate_set_active(&s_gate, false); clicker_gate_boundary_complete(&s_gate);
        s_host_active = false; ble_clicker_gap_event(&event);
        assert(!terminated && !s_peer_was_bonded && security_calls == 0);
        descriptor.sec_state.encrypted = descriptor.sec_state.bonded = true;
        allow_live_peer_delete = true;
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 0; ble_clicker_gap_event(&event);
        assert(delete_calls == 1 && terminated == 1 && ble_clicker_connection_closing(7));
        assert(status == PLATFORM_CLICKER_OFF && !s_hid_peer_valid);
        break;
    case 29: // Explicit exit still waits for key-up when encryption arrives late.
        ready(); assert(platform_clicker_send(CLICKER_NEXT)); work_event_cb(NULL);
        platform_clicker_set_active(false); work_event_cb(NULL);
        assert(s_exit_pending && ble_clicker_connection_closing(7) && !terminated);
        event.type = BLE_GAP_EVENT_ENC_CHANGE; event.enc_change.conn_handle = 7;
        event.enc_change.status = 0; ble_clicker_gap_event(&event);
        assert(!terminated && !delete_calls);
        process_report_tick(); assert(last_report[2] == 0 && !terminated);
        process_report_tick(); assert(terminated == 1);
        disconnected(); assert(strcmp(trace, "KRTDA") == 0 && !delete_calls);
        break;
    case 30: // Late/repeated passkey events preserve key-up and its settle timer.
        ready(); assert(platform_clicker_send(CLICKER_NEXT)); work_event_cb(NULL);
        platform_clicker_set_active(false);
        event.type = BLE_GAP_EVENT_PASSKEY_ACTION; event.passkey.conn_handle = 7;
        ble_clicker_gap_event(&event);
        assert(s_exit_pending && ble_clicker_connection_closing(7) && !terminated);
        process_report_tick();
        assert(last_report[2] == 0 && !terminated && s_exit_release_settle_pending);
        ble_clicker_gap_event(&event);
        assert(!terminated && s_exit_release_settle_pending);
        process_report_tick(); assert(terminated == 1);
        disconnected(); assert(strcmp(trace, "KRTDA") == 0 && !delete_calls);
        break;
    default: assert(false);
    }
    puts("clicker platform case PASS");
}
'''


class ClickerRunnerTest(unittest.TestCase):
    def test_default_keeps_address_and_undefined_sanitizers(self):
        with patch.dict(os.environ, {}, clear=True):
            self.assertEqual(host_sanitizers(), "address,undefined")

    def test_undefined_requires_explicit_environment_opt_in(self):
        with patch.dict(os.environ, {"PASSPORT_HOST_SANITIZERS": "undefined"}):
            self.assertEqual(host_sanitizers(), "undefined")

    def test_invalid_sanitizer_selection_is_rejected(self):
        for value in ("", "none", "address", "undefined,address", "undefined -O0"):
            with self.subTest(value=value), patch.dict(os.environ, {"PASSPORT_HOST_SANITIZERS": value}):
                with self.assertRaisesRegex(ValueError, "PASSPORT_HOST_SANITIZERS"):
                    host_sanitizers()

    def test_child_timeout_reports_unobserved_main_entry(self):
        with self.assertRaisesRegex(AssertionError, "main entry not observed"):
            run_host([sys.executable, "-c", "import time; time.sleep(5)"],
                     timeout=1, phase="case 9", sanitizers="address,undefined", check_main=True)

    def test_child_timeout_distinguishes_entered_case_body(self):
        child = "import time; print('PASSPORT_CLICKER_MAIN_ENTERED', flush=True); print('PASSPORT_CLICKER_CASE 9', flush=True); time.sleep(5)"
        with self.assertRaisesRegex(AssertionError, "case body entered"):
            run_host([sys.executable, "-c", child], timeout=1, phase="case 9",
                     sanitizers="undefined", check_main=True)

    def test_compile_failure_includes_compiler_output(self):
        with self.assertRaisesRegex(AssertionError, "(?s)compile.*compiler failure detail"):
            run_host([sys.executable, "-c", "import sys; print('compiler failure detail'); sys.exit(3)"],
                     timeout=1, phase="compile", sanitizers="undefined")


class ClickerPlatformTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sanitizers = host_sanitizers()
        print(f"Clicker host sanitizers={cls.sanitizers}" +
              (" (explicit opt-in; AddressSanitizer coverage disabled)"
               if cls.sanitizers == "undefined" else " (default ASAN + UBSan)"), file=sys.stderr)
        cls.directory = tempfile.TemporaryDirectory(prefix="passport-clicker-gap-")
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        source = SOURCE.read_text()
        code = BOUNDARY + "\n" + "\n".join(function(source, name) for name in (
            "desired_active", "change_requested", "capture_change_peer_host",
            "bond_replacement_allowed",
            "request_disconnect", "ble_clicker_connection_closing",
            "idle_status", "refresh_ready_status", "schedule_report_tick", "schedule_release_settle", "finish_exit",
            "process_report_tick", "activate_host", "deactivate_host", "work_event_cb",
            "platform_clicker_set_active", "platform_clicker_change_computer", "platform_clicker_send",
            "ble_clicker_pairing_paused_host", "ble_clicker_host_reset", "ble_clicker_host_synced",
            "ble_clicker_advertising_result_host", "ble_clicker_gap_event")) + CASES
        (directory / "callback.c").write_text(code)
        cls.executable = directory / "callback"
        run_host(["cc", "-std=c11", "-UNDEBUG", f"-fsanitize={cls.sanitizers}", "-fno-sanitize-recover=all", "-g",
                        "-I", str(ROOT / "components/core/services/include"),
                        "-I", str(ROOT / "components/platform/platform_esp32/include"),
                        str(directory / "callback.c"), str(ROOT / "components/core/services/src/clicker_gate.c"),
                        str(ROOT / "components/core/services/src/clicker_reports.c"), "-o", str(cls.executable)],
                 timeout=30, phase="compile", sanitizers=cls.sanitizers)

    def case(self, number):
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        run_host([str(self.executable), str(number)], timeout=5, phase=f"case {number}",
                 sanitizers=self.sanitizers, check_main=True, env=environment)

    def test_touch_only_release_and_switch(self): self.case(22)
    def test_lost_touch_subscription_cancels_input(self): self.case(23)
    def test_saved_phone_keeps_standard_ble_without_hid_or_repairing(self): self.case(24)
    def test_enotconn_cleanup_never_reterminates_connection(self): self.case(25)
    def test_failed_termination_retry_and_new_session_reset(self): self.case(26)
    def test_extra_rejected_handle_and_host_reset_are_isolated(self): self.case(27)
    def test_standard_mode_rejects_unexpected_new_bond(self): self.case(28)
    def test_late_encryption_during_explicit_exit_preserves_key_up(self): self.case(29)
    def test_late_passkey_during_explicit_exit_preserves_key_up_and_settle(self): self.case(30)

    def test_mouse_only_pairing_and_change_device_releases_drag(self): self.case(19)
    def test_keyboard_only_pairing_rejects_phone_swipe(self): self.case(20)
    def test_lost_mouse_subscription_cancels_queued_swipe(self): self.case(21)
    def test_saved_bond_without_connection_still_offers_pairing(self): self.case(1)
    def test_security_start_failure_frees_single_connection(self): self.case(2)
    def test_encryption_failure_frees_single_connection(self): self.case(3)
    def test_only_encrypted_hid_subscription_is_ready(self): self.case(4)
    def test_not_yet_advertising_does_not_offer_pairing(self): self.case(5)
    def test_failed_advertising_is_unavailable(self): self.case(6)
    def test_switch_releases_then_disconnects_then_deletes_only_current_peer(self): self.case(7)
    def test_switch_cancels_queued_old_keys(self): self.case(8)
    def test_disconnect_before_host_work_preserves_change_request(self): self.case(9)
    def test_switch_does_not_delete_a_configuration_clients_bonds(self): self.case(10)
    def test_leaving_mode_during_switch_does_not_reenter(self): self.case(11)
    def test_failed_peer_delete_pauses_advertising_until_retry(self): self.case(12)
    def test_extra_connection_cannot_replace_current_hid_peer(self): self.case(13)
    def test_host_reset_preserves_pending_current_peer_delete(self): self.case(14)
    def test_failed_release_uses_managed_disconnect(self): self.case(15)
    def test_switch_without_current_hid_peer_keeps_saved_bonds(self): self.case(16)
    def test_late_encryption_cannot_bypass_held_key_release(self): self.case(17)
    def test_pairing_completing_during_switch_uses_deferred_peer_delete(self): self.case(18)


if __name__ == "__main__":
    unittest.main()
