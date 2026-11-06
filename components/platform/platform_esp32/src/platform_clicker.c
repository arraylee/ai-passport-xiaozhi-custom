#include "platform/board_config.h"
#include "platform/platform_clicker.h"
#include "services/clicker_gate.h"

#include <string.h>
#include <stdio.h>

#if PERIPH_BLE

#include "ble_internal.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

static const char *TAG = "clicker_hid";

#define CLICKER_NAME "AI PASSPORT"
#define CLICKER_REPORT_DELAY_MS 25

static const ble_uuid16_t s_hid_svc_uuid = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t s_hid_info_uuid = BLE_UUID16_INIT(0x2a4a);
static const ble_uuid16_t s_report_map_uuid = BLE_UUID16_INIT(0x2a4b);
static const ble_uuid16_t s_control_point_uuid = BLE_UUID16_INIT(0x2a4c);
static const ble_uuid16_t s_report_uuid = BLE_UUID16_INIT(0x2a4d);
static const ble_uuid16_t s_report_ref_uuid = BLE_UUID16_INIT(0x2908);

/* Report ID 1: modifiers, reserved byte, then six USB HID key slots. */
static const uint8_t s_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x06,       /* Usage (Keyboard) */
    0xa1, 0x01,       /* Collection (Application) */
    0x85, 0x01,       /* Report ID (1) */
    0x05, 0x07,       /* Usage Page (Keyboard) */
    0x19, 0xe0,       /* Usage Minimum (Left Control) */
    0x29, 0xe7,       /* Usage Maximum (Right GUI) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x01,       /* Logical Maximum (1) */
    0x75, 0x01,       /* Report Size (1) */
    0x95, 0x08,       /* Report Count (8) */
    0x81, 0x02,       /* Input (Data, Variable, Absolute) */
    0x95, 0x01,       /* Report Count (1) */
    0x75, 0x08,       /* Report Size (8) */
    0x81, 0x03,       /* Input (Constant) */
    0x95, 0x06,       /* Report Count (6) */
    0x75, 0x08,       /* Report Size (8) */
    0x15, 0x00,       /* Logical Minimum (0) */
    0x25, 0x65,       /* Logical Maximum (101) */
    0x19, 0x00,       /* Usage Minimum (Reserved) */
    0x29, 0x65,       /* Usage Maximum (Keyboard Application) */
    0x81, 0x00,       /* Input (Data, Array, Absolute) */
    0xc0,             /* End keyboard collection */
    /* Report ID 2: standard relative mouse, buttons + int16 X/Y + wheel.
     * Android pointer input and iOS AssistiveTouch share this descriptor. */
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xa1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
    0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x05, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x16, 0x01, 0x80, 0x26, 0xff, 0x7f,
    0x75, 0x10, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
    /* Report ID 3: direct single-finger digitizer, five payload bytes.
     * Pen application makes Linux hid-generic set INPUT_PROP_DIRECT; Finger
     * physical collection maps InRange to BTN_TOOL_FINGER. A bare TouchScreen
     * + Stylus without Contact ID fell back to Android pointer mode. */
    0x05, 0x0d, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x03,
    0x09, 0x22, 0xa1, 0x00, /* Finger physical collection */
    0x09, 0x42, 0x09, 0x32, /* Tip Switch, In Range */
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x06, 0x81, 0x03,
    0x05, 0x01, 0x09, 0x01, 0xa1, 0x00,
    0x09, 0x30, 0x09, 0x31,
    0x15, 0x00, 0x26, 0x10, 0x27, /* Logical range 0..10000 */
    0x35, 0x00, 0x46, 0x10, 0x27, /* Physical range 0..10000 */
    0x55, 0x00, 0x65, 0x00, /* Unit exponent 0, unit none */
    0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0xc0, 0xc0, 0xc0,
};

/* A single-contact absolute digitizer: tip/in-range + X/Y in 0..10000.
 * Android maps these coordinates to its display, independent of mouse speed.
 * No vendor-specific reports, host resolution, or OS fingerprinting. */
static const uint8_t s_hid_info[] = {0x11, 0x01, 0x00, 0x00};
static const uint8_t s_report_ref[] = {0x01, 0x01};
static const uint8_t s_mouse_report_ref[] = {0x02, 0x01};
static const uint8_t s_touch_report_ref[] = {0x03, 0x01};
static uint8_t s_control_point;
static uint16_t s_report_handle, s_mouse_report_handle, s_touch_report_handle;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static platform_clicker_snapshot_t s_snapshot = {
    .status = PLATFORM_CLICKER_UNAVAILABLE,
    .name = CLICKER_NAME,
};
static clicker_gate_t s_gate;
/* Cross-task request, protected by s_lock. */
static bool s_change_requested;

/* The fields below are owned by the NimBLE host task. */
static clicker_reports_t s_reports;
static struct ble_npl_event s_work_event;
static struct ble_npl_callout s_report_callout;
static esp_timer_handle_t s_bridge_timer;
static bool s_initialized;
static bool s_host_synced;
static bool s_report_callout_ready;
static bool s_host_active;
static bool s_advertising;
static int s_advertising_error;
static bool s_exit_pending;
static bool s_exit_release_settle_pending;
static bool s_shutting_down;
static bool s_connected;
static bool s_encrypted;
static bool s_subscribed, s_mouse_subscribed, s_touch_subscribed;
static bool s_peer_was_bonded;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_conn_closing;
/* Rejecting a late extra connection must not replace the live session handle. */
static uint16_t s_rejected_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_report_generation;
static ble_addr_t s_hid_peer, s_change_peer;
static bool s_hid_peer_valid, s_change_peer_valid, s_change_peer_captured;

static bool change_requested(void)
{
    bool pending;
    portENTER_CRITICAL(&s_lock);
    pending = s_change_requested;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

static void capture_change_peer_host(void)
{
    /* Also called before disconnect/reset clears the current identity: a GAP
     * event can arrive before the queued menu request reaches work_event_cb. */
    if (!change_requested() || s_change_peer_captured) return;
    s_change_peer_captured = true;
    s_change_peer_valid = s_hid_peer_valid;
    if (s_hid_peer_valid) s_change_peer = s_hid_peer;
}


static bool desired_active(void)
{
    bool active;
    portENTER_CRITICAL(&s_lock);
    active = clicker_gate_desired_active(&s_gate);
    portEXIT_CRITICAL(&s_lock);
    return active;
}

static bool exit_latched(void)
{
    bool pending;
    portENTER_CRITICAL(&s_lock);
    pending = s_gate.exit_latched;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

static int request_disconnect(uint16_t conn_handle)
{
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) return BLE_HS_ENOTCONN;
    if ((conn_handle == s_conn_handle && s_conn_closing) ||
        conn_handle == s_rejected_conn_handle) return BLE_HS_EALREADY;
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        if (conn_handle == s_conn_handle) s_conn_closing = true;
        else s_rejected_conn_handle = conn_handle;
    }
    return rc;
}

bool ble_clicker_connection_closing(uint16_t conn_handle)
{
    if (!s_initialized || conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    if (conn_handle == s_rejected_conn_handle) return true;
    return conn_handle == s_conn_handle &&
           (s_conn_closing || s_exit_pending || exit_latched() ||
            change_requested() || s_reports.disconnect_required);
}

static void snapshot_set_status(platform_clicker_status_t status)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.status = status;
    portEXIT_CRITICAL(&s_lock);
}

static void snapshot_sync_counts(void)
{
    portENTER_CRITICAL(&s_lock);
    s_snapshot.sent = s_reports.sent;
    s_snapshot.failed = s_reports.failed;
    portEXIT_CRITICAL(&s_lock);
}

static void input_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    clicker_gate_input_done(&s_gate);
    portEXIT_CRITICAL(&s_lock);
}

static bool bond_replacement_allowed(void)
{
    bool allowed;
    portENTER_CRITICAL(&s_lock);
    allowed = clicker_gate_bond_replacement_allowed(
        &s_gate, s_host_active, s_exit_pending, s_shutting_down);
    portEXIT_CRITICAL(&s_lock);
    return allowed;
}

static int store_status(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    clicker_store_policy_t policy;
    portENTER_CRITICAL(&s_lock);
    policy = clicker_gate_store_policy(
        &s_gate, s_host_active, s_exit_pending, s_shutting_down,
        event->event_code == BLE_STORE_EVENT_OVERFLOW);
    portEXIT_CRITICAL(&s_lock);
    if (policy == CLICKER_STORE_REPLACE) {
        int rc = ble_store_util_status_rr(event, NULL);
        ESP_LOGI(TAG, "HID store overflow type=%d scan=%d adv=%d recovery=%d",
                 event->overflow.obj_type, ble_gap_disc_active(), ble_gap_adv_active(), rc);
        return rc;
    }
    /* A FULL event is only predictive. Standard mode hides secured HOGP
     * services, so allow non-bonding encryption to proceed. Any real
     * overflow must fail without evicting the presentation-computer bond. */
    return policy == CLICKER_STORE_PROCEED ? 0 : BLE_HS_ESTORE_CAP;
}

static bool peer_is_bonded(const ble_addr_t *peer_id_addr)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    if (ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS) != 0) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (peers[i].type == peer_id_addr->type &&
            memcmp(peers[i].val, peer_id_addr->val, sizeof peers[i].val) == 0) {
            return true;
        }
    }
    return false;
}

static platform_clicker_status_t idle_status(void)
{
    /* A stored bond is not a live connection. Undirected advertising still
     * accepts pairing from another computer while the saved one is absent. */
    return s_advertising ? PLATFORM_CLICKER_PAIRING : PLATFORM_CLICKER_CONNECTING;
}

static void refresh_ready_status(void)
{
    portENTER_CRITICAL(&s_lock);
    bool ready = s_host_active && !s_exit_pending && !s_conn_closing && !s_shutting_down &&
                 s_connected && s_encrypted && !s_advertising_error;
    s_snapshot.keyboard_ready = ready && s_subscribed;
    s_snapshot.mouse_ready = ready && s_mouse_subscribed;
    s_snapshot.touch_ready = ready && s_touch_subscribed;
    portEXIT_CRITICAL(&s_lock);
    if (change_requested() && desired_active() && !s_shutting_down) {
        snapshot_set_status(s_advertising_error ? PLATFORM_CLICKER_UNAVAILABLE
                                               : PLATFORM_CLICKER_SWITCHING);
    } else if (!s_host_active || s_exit_pending || s_shutting_down) {
        snapshot_set_status(s_shutting_down ? PLATFORM_CLICKER_UNAVAILABLE : PLATFORM_CLICKER_OFF);
    } else if (s_advertising_error != 0) {
        snapshot_set_status(PLATFORM_CLICKER_UNAVAILABLE);
    } else if (!s_conn_closing && s_connected && s_encrypted && (s_subscribed || s_mouse_subscribed || s_touch_subscribed)) {
        snapshot_set_status(PLATFORM_CLICKER_READY);
    } else if (s_connected) {
        snapshot_set_status(s_peer_was_bonded ? PLATFORM_CLICKER_CONNECTING
                                              : PLATFORM_CLICKER_PAIRING);
    } else {
        snapshot_set_status(idle_status());
    }
}

static bool hid_access_allowed(void)
{
    return (s_host_active || s_exit_pending) && !s_shutting_down;
}

static int append_value(struct ble_gatt_access_ctxt *ctxt,
                        const void *data, size_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hid_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    if (!hid_access_allowed()) {
        return ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR
                   ? BLE_ATT_ERR_WRITE_NOT_PERMITTED
                   : BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    uintptr_t kind = (uintptr_t)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (kind) {
        case 1:
            ESP_LOGI(TAG, "HID report map read bytes=%u", (unsigned)sizeof s_report_map);
            return append_value(ctxt, s_report_map, sizeof s_report_map);
        case 2: return append_value(ctxt, clicker_reports_current(&s_reports), 8);
        case 5: return append_value(ctxt, clicker_reports_mouse_current(&s_reports), 6);
        case 6: return append_value(ctxt, clicker_reports_touch_current(&s_reports), 5);
        case 3: return append_value(ctxt, s_hid_info, sizeof s_hid_info);
        default: return BLE_ATT_ERR_READ_NOT_PERMITTED;
        }
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && kind == 4) {
        uint16_t copied = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) != 1 ||
            ble_hs_mbuf_to_flat(ctxt->om, &s_control_point, 1, &copied) != 0 ||
            copied != 1 || s_control_point > 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int report_ref_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (!hid_access_allowed()) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    ESP_LOGI(TAG, "HID report reference read id=%u", (uintptr_t)arg == 3 ? 3u : (uintptr_t)arg == 2 ? 2u : 1u);
    return append_value(ctxt, (uintptr_t)arg == 3 ? s_touch_report_ref : (uintptr_t)arg == 2 ? s_mouse_report_ref : s_report_ref, 2);
}

static struct ble_gatt_dsc_def s_report_descriptors[] = {
    {
        .uuid = &s_report_ref_uuid.u,
        .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
        .access_cb = report_ref_access,
    },
    {0},
};

static struct ble_gatt_dsc_def s_mouse_report_descriptors[] = {
    {.uuid = &s_report_ref_uuid.u, .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
     .access_cb = report_ref_access, .arg = (void *)2},
    {0},
};

static struct ble_gatt_dsc_def s_touch_report_descriptors[] = {
    {.uuid = &s_report_ref_uuid.u, .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
     .access_cb = report_ref_access, .arg = (void *)3},
    {0},
};

static struct ble_gatt_chr_def s_hid_characteristics[] = {
    {
        .uuid = &s_report_map_uuid.u,
        .access_cb = hid_access,
        .arg = (void *)1,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
        .min_key_size = 16,
    },
    {
        .uuid = &s_report_uuid.u,
        .access_cb = hid_access,
        .arg = (void *)2,
        .descriptors = s_report_descriptors,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
        .min_key_size = 16,
        .val_handle = &s_report_handle,
    },
    {
        .uuid = &s_report_uuid.u,
        .access_cb = hid_access,
        .arg = (void *)5,
        .descriptors = s_mouse_report_descriptors,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
        .min_key_size = 16,
        .val_handle = &s_mouse_report_handle,
    },
    {
        .uuid = &s_report_uuid.u,
        .access_cb = hid_access,
        .arg = (void *)6,
        .descriptors = s_touch_report_descriptors,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
        .min_key_size = 16,
        .val_handle = &s_touch_report_handle,
    },
    {
        .uuid = &s_hid_info_uuid.u,
        .access_cb = hid_access,
        .arg = (void *)3,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
        .min_key_size = 16,
    },
    {
        .uuid = &s_control_point_uuid.u,
        .access_cb = hid_access,
        .arg = (void *)4,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
        .min_key_size = 16,
    },
    {0},
};

static const struct ble_gatt_svc_def s_hid_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_hid_svc_uuid.u,
        .characteristics = s_hid_characteristics,
    },
    {0},
};

static bool send_report(const uint8_t report[8], void *user)
{
    (void)user;
    bool mouse = clicker_reports_id(&s_reports) == 2;
    bool touch = clicker_reports_id(&s_reports) == 3;
    uint16_t handle = touch ? s_touch_report_handle : mouse ? s_mouse_report_handle : s_report_handle;
    size_t length = touch ? 5 : mouse ? 6 : 8;
    if ((!s_host_active && !s_exit_pending) || s_conn_closing || !s_connected || !s_encrypted ||
        !(touch ? s_touch_subscribed : mouse ? s_mouse_subscribed : s_subscribed) || s_conn_handle == BLE_HS_CONN_HANDLE_NONE ||
        handle == 0 || s_shutting_down) {
        return false;
    }

    bool release = true;
    for (size_t i = 0; i < 8; ++i) release = release && report[i] == 0;
    /* Touch-up retains endpoint coordinates, so it is not an all-zero packet.
     * It must still escape the exit generation boundary to prevent a held touch. */
    if (touch) release = report[0] == 0;
    if (!release) {
        bool current;
        portENTER_CRITICAL(&s_lock);
        current = clicker_gate_action_current(&s_gate, s_report_generation);
        portEXIT_CRITICAL(&s_lock);
        if (!current) return false;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(report, length);
    if (om == NULL) return false;
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "HID notify failed rc=%d", rc);
        return false;
    }
    return true;
}

static void schedule_report_tick(void)
{
    (void)ble_npl_callout_reset(&s_report_callout,
        ble_npl_time_ms_to_ticks32(CLICKER_REPORT_DELAY_MS));
}

static void schedule_release_settle(void)
{
    uint32_t delay_ms = CLICKER_REPORT_DELAY_MS;
    struct ble_gap_conn_desc desc;
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
        ble_gap_conn_find(s_conn_handle, &desc) == 0) {
        /* Connection interval units are 1.25 ms. Respect the negotiated
         * interval/latency instead of assuming every computer accepts 25 ms. */
        uint32_t interval_us = (uint32_t)desc.conn_itvl * 1250U *
                               ((uint32_t)desc.conn_latency + 1U);
        uint32_t settle_ms = (interval_us + 999U) / 1000U + 5U;
        if (settle_ms > delay_ms) delay_ms = settle_ms;
    }
    (void)ble_npl_callout_reset(&s_report_callout, ble_npl_time_ms_to_ticks32(delay_ms));
}

static void finish_exit(bool restart_advertising)
{
    s_exit_release_settle_pending = false;
    input_clear();
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        /* Keep EXITING latched until GAP disconnect confirms the old session
         * is gone. A fast re-enter starts only after that boundary. */
        ble_config_clicker_mode_changed_host();
        return;
    }

    capture_change_peer_host();
    if (s_change_peer_valid) {
        /* The address was captured from this HID connection, never from the
         * global bond list. Retry this same peer if a store write fails. */
        int rc = ble_store_util_delete_peer(&s_change_peer);
        if (rc != 0) {
            s_advertising_error = rc;
            s_exit_pending = true;
            s_host_active = false;
            snapshot_set_status(PLATFORM_CLICKER_UNAVAILABLE);
            (void)ble_npl_callout_reset(&s_report_callout, ble_npl_time_ms_to_ticks32(250));
            return;
        }
    }
    s_advertising_error = 0;
    s_exit_pending = false;
    bool reenter;
    portENTER_CRITICAL(&s_lock);
    if (s_change_requested && !s_change_peer_captured) {
        /* A new menu request arrived while a prior exit was completing. Keep
         * its boundary; capture and process it on the next host tick. */
        portEXIT_CRITICAL(&s_lock);
        s_exit_pending = true;
        s_host_active = false;
        schedule_report_tick();
        return;
    }
    s_change_peer_valid = s_change_peer_captured = false;
    s_change_requested = false;
    reenter = clicker_gate_boundary_complete(&s_gate);
    portEXIT_CRITICAL(&s_lock);
    s_host_active = reenter;
    ble_hs_cfg.sm_bonding = reenter ? 1 : 0;
    clicker_reports_disconnected(&s_reports);
    refresh_ready_status();
    if (restart_advertising) ble_config_clicker_mode_changed_host();
}

static void process_report_tick(void)
{
    uint32_t sent_before = s_reports.sent, failed_before = s_reports.failed;
    clicker_reports_tick(&s_reports, send_report, NULL);
    if (sent_before != s_reports.sent || failed_before != s_reports.failed)
        ESP_LOGI(TAG, "HID gesture report=%u sent=%lu failed=%lu", clicker_reports_id(&s_reports),
                 (unsigned long)s_reports.sent, (unsigned long)s_reports.failed);
    snapshot_sync_counts();
    if (s_reports.disconnect_required) {
        input_clear();
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            /* Shared mode transition retries failed termination requests. */
            ble_config_clicker_mode_changed_host();
        } else {
            clicker_reports_disconnected(&s_reports);
            if (s_exit_pending) finish_exit(true);
        }
        return;
    }
    if (clicker_reports_busy(&s_reports)) {
        schedule_report_tick();
        return;
    }
    input_clear();
    if (s_exit_pending) {
        if (s_exit_release_settle_pending) {
            finish_exit(true);
        } else {
            /* Give the all-zero notification one connection interval to leave
             * the controller before terminating the HID connection. */
            s_exit_release_settle_pending = true;
            schedule_release_settle();
        }
    }
}

static void report_callout_cb(struct ble_npl_event *event)
{
    (void)event;
    if (s_shutting_down) return;
    process_report_tick();
}

static void activate_host(bool switch_mode)
{
    s_host_active = true;
    s_advertising = false;
    s_advertising_error = 0;
    s_exit_pending = false;
    s_exit_release_settle_pending = false;
    s_encrypted = false;
    s_subscribed = s_mouse_subscribed = s_touch_subscribed = false;
    ble_hs_cfg.sm_bonding = 1;
    clicker_reports_disconnected(&s_reports);
    input_clear();
    refresh_ready_status();
    if (switch_mode) ble_config_clicker_mode_changed_host();
}

static void deactivate_host(void)
{
    s_host_active = false;
    s_exit_pending = true;
    s_exit_release_settle_pending = false;
    ble_hs_cfg.sm_bonding = 0;
    clicker_reports_cancel(&s_reports);
    snapshot_set_status(!desired_active() ? PLATFORM_CLICKER_OFF :
                        change_requested() ? PLATFORM_CLICKER_SWITCHING : PLATFORM_CLICKER_CONNECTING);
    if (clicker_reports_busy(&s_reports)) {
        schedule_report_tick();
    } else {
        finish_exit(true);
    }
}

static void work_event_cb(struct ble_npl_event *event)
{
    (void)event;
    if (!s_host_synced || s_shutting_down) return;

    capture_change_peer_host();
    clicker_gate_work_t work;
    portENTER_CRITICAL(&s_lock);
    clicker_gate_take_work(&s_gate, &work);
    portEXIT_CRITICAL(&s_lock);

    if (work.begin_exit) {
        if (!s_exit_pending) deactivate_host();
        return;
    }

    if (work.desired_active && !s_host_active && !s_exit_pending) {
        activate_host(true);
    }

    if (!work.has_action) {
        refresh_ready_status();
        return;
    }
    bool current;
    portENTER_CRITICAL(&s_lock);
    current = clicker_gate_action_current(&s_gate, work.action_generation);
    portEXIT_CRITICAL(&s_lock);
    s_report_generation = work.action_generation;
    ESP_LOGI(TAG, "HID action=%d connected=%d encrypted=%d keyboard=%d mouse=%d touch=%d",
             work.action, s_connected, s_encrypted, s_subscribed, s_mouse_subscribed, s_touch_subscribed);
    if (!s_host_active || s_exit_pending || !s_connected || !s_encrypted ||
        !((work.action == CLICKER_TOUCH_PREV || work.action == CLICKER_TOUCH_NEXT) ? s_touch_subscribed : work.action >= CLICKER_PHONE_PREV ? s_mouse_subscribed : s_subscribed) ||
        !current || !clicker_reports_request(&s_reports, work.action, true)) {
        input_clear();
        return;
    }
    process_report_tick();
}

static void bridge_timer_cb(void *arg)
{
    (void)arg;
    if (!s_initialized || s_shutting_down || ble_npl_event_is_queued(&s_work_event)) return;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_work_event);
}

static void schedule_work(void)
{
    if (!s_initialized || s_shutting_down || s_bridge_timer == NULL) return;
    esp_err_t rc = esp_timer_start_once(s_bridge_timer, 1);
    if (rc != ESP_OK && rc != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "failed to marshal host work: %s", esp_err_to_name(rc));
    }
}

void platform_clicker_set_active(bool active)
{
    portENTER_CRITICAL(&s_lock);
    clicker_gate_set_active(&s_gate, active);
    if (!active) {
        s_snapshot.status = PLATFORM_CLICKER_OFF;
    } else if (!s_initialized || s_shutting_down) {
        s_snapshot.status = PLATFORM_CLICKER_UNAVAILABLE;
    } else if (s_snapshot.status == PLATFORM_CLICKER_OFF) {
        s_snapshot.status = PLATFORM_CLICKER_CONNECTING;
    }
    portEXIT_CRITICAL(&s_lock);
    schedule_work();
}

bool platform_clicker_change_computer(void)
{
    bool accepted = false;
    portENTER_CRITICAL(&s_lock);
    if (s_initialized && !s_shutting_down && !s_change_requested &&
        clicker_gate_desired_active(&s_gate)) {
        s_change_requested = true;
        /* Reuse the generation boundary: discard queued keys immediately,
         * preserve a held key's release, and reenter only after disconnect. */
        clicker_gate_set_active(&s_gate, false);
        clicker_gate_set_active(&s_gate, true);
        s_snapshot.status = PLATFORM_CLICKER_SWITCHING;
        accepted = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) schedule_work();
    return accepted;
}

bool platform_clicker_send(clicker_action_t action)
{
    bool accepted = false;
    portENTER_CRITICAL(&s_lock);
    if (s_initialized && !s_shutting_down) {
        accepted = clicker_gate_enqueue(
            &s_gate, action, s_snapshot.status == PLATFORM_CLICKER_READY &&
                ((action == CLICKER_TOUCH_PREV || action == CLICKER_TOUCH_NEXT) ? s_snapshot.touch_ready : action >= CLICKER_PHONE_PREV ? s_snapshot.mouse_ready : s_snapshot.keyboard_ready));
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) schedule_work();
    return accepted;
}

void platform_clicker_snapshot(platform_clicker_snapshot_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
}

static int clicker_init_name(void)
{
    // Stable hardware suffix, independent of profile nicknames and BLE mode.
    // Use the base Wi-Fi MAC also used to derive the normal device SN, rather
    // than the BLE address (+2). Initialize once before the host/worker starts.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        ESP_LOGE(TAG, "hardware identity unavailable; clicker disabled");
        return BLE_HS_EINVAL;
    }
    snprintf(s_snapshot.name, sizeof s_snapshot.name, CLICKER_NAME " %02X%02X%02X",
             (unsigned)mac[3], (unsigned)mac[4], (unsigned)mac[5]);
    return 0;
}

int ble_clicker_init(void)
{
    ble_clicker_services_reset();
    s_shutting_down = false;
    s_host_synced = false;
    int rc = clicker_init_name();
    if (rc != 0) return rc;
    clicker_gate_init(&s_gate);
    clicker_reports_init(&s_reports);
    ble_npl_event_init(&s_work_event, work_event_cb, NULL);
    ble_npl_callout_init(&s_report_callout, nimble_port_get_dflt_eventq(),
                         report_callout_cb, NULL);
    s_report_callout_ready = true;

    const esp_timer_create_args_t timer_args = {
        .callback = bridge_timer_cb,
        .name = "clicker_ble",
    };
    if (esp_timer_create(&timer_args, &s_bridge_timer) != ESP_OK) {
        ESP_LOGE(TAG, "host bridge timer unavailable");
        return BLE_HS_ENOMEM;
    }

    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = store_status;

    rc = ble_gatts_count_cfg(s_hid_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_hid_services);
    if (rc == 0) rc = ble_clicker_device_info_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "HID GATT registration failed rc=%d", rc);
        return rc;
    }

    s_initialized = true;
    snapshot_set_status(PLATFORM_CLICKER_OFF);
    ESP_LOGI(TAG, "NimBLE HOGP keyboard + mouse + touch + DIS/PnP + BAS registered as %s", ble_clicker_name());
    return 0;
}

void ble_clicker_host_synced(void)
{
    if (!s_initialized || s_shutting_down) return;
    s_host_synced = true;
    if (change_requested()) {
        s_exit_pending = true;
        finish_exit(false);
        return;
    }
    s_advertising = false;
    s_advertising_error = 0;
    portENTER_CRITICAL(&s_lock);
    s_host_active = clicker_gate_boundary_complete(&s_gate);
    portEXIT_CRITICAL(&s_lock);
    s_exit_pending = false;
    s_exit_release_settle_pending = false;
    ble_hs_cfg.sm_bonding = s_host_active ? 1 : 0;
    clicker_reports_disconnected(&s_reports);
    refresh_ready_status();
}

void ble_clicker_host_reset(void)
{
    capture_change_peer_host();
    ble_npl_callout_stop(&s_report_callout);
    s_hid_peer_valid = false;
    s_host_synced = false;
    s_advertising = false;
    s_advertising_error = 0;
    s_host_active = false;
    s_exit_pending = false;
    s_connected = false;
    s_encrypted = false;
    s_subscribed = s_mouse_subscribed = s_touch_subscribed = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_conn_closing = false;
    s_rejected_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_exit_release_settle_pending = false;
    ble_hs_cfg.sm_bonding = 0;
    clicker_reports_disconnected(&s_reports);
    portENTER_CRITICAL(&s_lock);
    clicker_gate_reset_io(&s_gate);
    bool want_active = clicker_gate_desired_active(&s_gate);
    portEXIT_CRITICAL(&s_lock);
    snapshot_set_status(want_active && !s_shutting_down
                            ? PLATFORM_CLICKER_CONNECTING
                            : PLATFORM_CLICKER_OFF);
}

bool ble_clicker_pairing_paused_host(void)
{
    return change_requested();
}

bool ble_clicker_advertising_active(void)
{
    return s_initialized && s_host_active && !s_exit_pending && !s_shutting_down;
}

void ble_clicker_advertising_result_host(bool active, int error)
{
    if (!s_initialized) return;
    s_advertising = active;
    s_advertising_error = error;
    refresh_ready_status();
}

const char *ble_clicker_name(void)
{
    return s_snapshot.name;
}

int ble_clicker_gap_event(struct ble_gap_event *event)
{
    if (!s_initialized) return 0;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        s_advertising = false;
        s_advertising_error = 0;
        if (event->connect.status != 0) break;
        /* Reject a late second/old connection without replacing the active
         * identity used by change-computer and key-release processing. */
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE &&
            s_conn_handle != event->connect.conn_handle) {
            (void)request_disconnect(event->connect.conn_handle);
            break;
        }
        s_conn_handle = event->connect.conn_handle;
        s_conn_closing = false;
        if (s_rejected_conn_handle == s_conn_handle) s_rejected_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = true;
        s_encrypted = false;
        s_subscribed = s_mouse_subscribed = s_touch_subscribed = false;
        s_exit_release_settle_pending = false;
        s_peer_was_bonded = false;
        s_hid_peer_valid = false;
        if (ble_gap_conn_find(s_conn_handle, &desc) == 0) {
            s_peer_was_bonded = peer_is_bonded(&desc.peer_id_addr);
            s_encrypted = desc.sec_state.encrypted;
            if (s_host_active && !s_exit_pending && s_peer_was_bonded) {
                s_hid_peer = desc.peer_id_addr;
                s_hid_peer_valid = true;
            }
        }
        ESP_LOGI(TAG, "HID connected active=%d bonded=%d encrypted=%d",
                 s_host_active, s_peer_was_bonded, s_encrypted);
        if (change_requested() || s_exit_pending || exit_latched()) {
            (void)request_disconnect(s_conn_handle);
        } else if (s_host_active && !s_exit_pending) {
            refresh_ready_status();
            int rc = ble_gap_security_initiate(s_conn_handle);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "security initiate failed rc=%d", rc);
                /* Release the single connection so a failed attempt does not
                 * hide the keyboard from the computer's Bluetooth search. */
                (void)request_disconnect(s_conn_handle);
            }
        }
        /* Standard-mode reconnects may reuse a saved phone bond. Keep the
         * ordinary BLE link; HID readiness and report transmission remain
         * gated by s_host_active, and pairing replacement remains disabled. */
        refresh_ready_status();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_rejected_conn_handle)
            s_rejected_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        if (event->disconnect.conn.conn_handle != s_conn_handle) break;
        capture_change_peer_host();
        s_hid_peer_valid = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_conn_closing = false;
        s_connected = false;
        s_encrypted = false;
        s_subscribed = s_mouse_subscribed = s_touch_subscribed = false;
        s_exit_release_settle_pending = false;
        clicker_reports_disconnected(&s_reports);
        input_clear();
        if (s_exit_pending || exit_latched() || change_requested()) {
            /* ble_config clears its handle and queues advertising after this
             * callback; do not restart or terminate using that stale handle. */
            finish_exit(false);
        }
        refresh_ready_status();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn_handle &&
            (event->subscribe.attr_handle == s_report_handle ||
             event->subscribe.attr_handle == s_mouse_report_handle ||
             event->subscribe.attr_handle == s_touch_report_handle)) {
            if (event->subscribe.attr_handle == s_mouse_report_handle)
                s_mouse_subscribed = event->subscribe.cur_notify != 0;
            else if (event->subscribe.attr_handle == s_touch_report_handle)
                s_touch_subscribed = event->subscribe.cur_notify != 0;
            else s_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "HID subscribe report=%u enabled=%d encrypted=%d",
                     event->subscribe.attr_handle == s_touch_report_handle ? 3u :
                     event->subscribe.attr_handle == s_mouse_report_handle ? 2u : 1u,
                     event->subscribe.cur_notify, s_encrypted);
            if (s_host_active && !s_exit_pending && s_encrypted && (s_subscribed || s_mouse_subscribed || s_touch_subscribed) &&
                ble_gap_conn_find(s_conn_handle, &desc) == 0) {
                s_hid_peer = desc.peer_id_addr;
                s_hid_peer_valid = true;
            }
            refresh_ready_status();
        }
        break;
    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.conn_handle == s_conn_handle &&
            (event->notify_tx.attr_handle == s_report_handle ||
             event->notify_tx.attr_handle == s_mouse_report_handle ||
             event->notify_tx.attr_handle == s_touch_report_handle)) {
            // Stack completion proves transmission only, not app gesture handling.
            ESP_LOGI(TAG, "HID TX report=%u status=%d",
                     event->notify_tx.attr_handle == s_touch_report_handle ? 3u :
                     event->notify_tx.attr_handle == s_mouse_report_handle ? 2u : 1u,
                     event->notify_tx.status);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle != s_conn_handle) break;
        s_encrypted = false;
        if (event->enc_change.status == BLE_HS_ENOTCONN) {
            /* NimBLE emits this while cancelling an in-progress SM procedure,
             * before GAP DISCONNECT. No second terminate/security retry. */
            s_conn_closing = true;
            refresh_ready_status();
            break;
        }
        if (event->enc_change.status != 0 && s_host_active && !s_exit_pending) {
            ESP_LOGW(TAG, "HID encryption failed status=%d", event->enc_change.status);
            (void)request_disconnect(s_conn_handle);
        }
        if (event->enc_change.status == 0 && ble_gap_conn_find(s_conn_handle, &desc) == 0) {
            s_encrypted = desc.sec_state.encrypted;
            if (s_host_active && !s_exit_pending && !exit_latched() &&
                (desc.sec_state.bonded || s_subscribed || s_mouse_subscribed || s_touch_subscribed)) {
                s_hid_peer = desc.peer_id_addr;
                s_hid_peer_valid = true;
            }
            if (change_requested()) {
                /* Pairing may finish after the menu request. Keep that peer's
                 * new keys until key-up and GAP disconnect, then remove only
                 * this connection's identity through the same retry path. */
                if (desc.sec_state.bonded && !s_change_peer_valid) {
                    s_hid_peer = desc.peer_id_addr;
                    s_hid_peer_valid = true;
                    s_change_peer = desc.peer_id_addr;
                    s_change_peer_valid = s_change_peer_captured = true;
                }
                if (!s_exit_pending) {
                    capture_change_peer_host();
                    deactivate_host();
                }
                refresh_ready_status();
                break;
            }
            bool boundary_requested = s_exit_pending || exit_latched();
            if (boundary_requested || !s_host_active) {
                bool new_bond = !s_peer_was_bonded && desc.sec_state.bonded;
                if (new_bond) {
                    (void)ble_store_util_delete_peer(&desc.peer_id_addr);
                }
                if (boundary_requested) {
                    /* A late encryption event must not bypass queued key-up
                     * and its settle interval on an explicit exit. */
                    if (!s_exit_pending) deactivate_host();
                } else if (new_bond) {
                    (void)request_disconnect(s_conn_handle);
                }
            }
        }
        ESP_LOGI(TAG, "HID encryption status=%d enabled=%d", event->enc_change.status, s_encrypted);
        refresh_ready_status();
        break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (s_exit_pending || exit_latched()) {
            if (event->passkey.conn_handle == s_conn_handle) {
                /* Preserve the same key-up/settle boundary as ENC_CHANGE;
                 * repeated security events must not restart that boundary. */
                if (!s_exit_pending) deactivate_host();
            } else {
                (void)request_disconnect(event->passkey.conn_handle);
            }
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (!bond_replacement_allowed()) return BLE_GAP_REPEAT_PAIRING_IGNORE;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            (void)ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    default:
        break;
    }
    return 0;
}

void ble_clicker_begin_shutdown(void)
{
    s_shutting_down = true;
    ble_hs_cfg.sm_bonding = 0;
    portENTER_CRITICAL(&s_lock);
    clicker_gate_set_active(&s_gate, false);
    clicker_gate_reset_io(&s_gate);
    s_snapshot.status = PLATFORM_CLICKER_UNAVAILABLE;
    portEXIT_CRITICAL(&s_lock);
    if (s_bridge_timer != NULL) (void)esp_timer_stop(s_bridge_timer);
}

// Called after nimble_port_stop has drained the host; no callback can run now.
void ble_clicker_deinit_after_host_stop(void)
{
    if (s_bridge_timer) {
        esp_timer_stop(s_bridge_timer);
        esp_timer_delete(s_bridge_timer);
        s_bridge_timer = NULL;
    }
    if (s_report_callout_ready) {
        ble_clicker_host_reset();
        ble_npl_callout_deinit(&s_report_callout);
        ble_npl_event_deinit(&s_work_event);
        s_report_callout_ready = false;
    }
    s_initialized = false;
}

#else

void platform_clicker_set_active(bool active) { (void)active; }
bool platform_clicker_send(clicker_action_t action) { (void)action; return false; }
bool platform_clicker_change_computer(void) { return false; }
void platform_clicker_snapshot(platform_clicker_snapshot_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof *out);
    out->status = PLATFORM_CLICKER_UNAVAILABLE;
    strncpy(out->name, "AI PASSPORT", sizeof out->name - 1);
}

#endif
