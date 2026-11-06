// components/platform_esp32/src/ble_config.c —— NimBLE GATT:结构化 JSON 配置通道
// 命令写特征(重组分帧) + notify 状态特征 + 身份只读 + 游戏积分只读。
#include "platform/board_config.h"
#if PERIPH_BLE
#include "platform/platform_factory.h"
#include "hal/hal_config.h"
#include "services/frame_reasm.h"
#include <string.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"   // ble_att_mtu / ble_att_set_preferred_mtu
#include "host/ble_gatt.h"  // ble_gattc_exchange_mtu(主动发起 MTU 协商)
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_internal.h"

static const char *TAG = "ble_config";

// UUID 基址:ASCII "TRAECARD" + 末字节字段号。NimBLE 用小端字节序存储。
#define TRAE_UUID128(nn) BLE_UUID128_INIT( \
    (nn),0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x44,0x52,0x41,0x43,0x45,0x41,0x52,0x54)

static const ble_uuid128_t g_svc_uuid    = TRAE_UUID128(0x00);
static const ble_uuid128_t g_cmd_uuid    = TRAE_UUID128(0x10);   // 命令写特征
static const ble_uuid128_t g_notify_uuid = TRAE_UUID128(0x11);   // notify 状态特征
static const ble_uuid128_t g_ident_uuid  = TRAE_UUID128(0x12);   // 身份只读特征
static const ble_uuid128_t g_game_uuid   = TRAE_UUID128(0x13);   // 游戏积分只读特征
static const ble_uuid128_t g_capture_uuid = TRAE_UUID128(0x14);  // 屏幕截图 notify 特征

// 广播用的设备名。改为运行时传入(内容是本机 SN),这样网关扫一下就知道是哪张卡,
// 不必再靠台账反查 —— 也就绕开了"广播地址 = WIFI MAC + 2"那个坑。
// ble_config.c 仍然不认识身份模块,名字由组装根 app.c 传进来。
static char s_dev_name[32] = BLE_DEVICE_NAME;

static const uint8_t *s_ident_ptr;
static int            s_ident_len;
static const uint8_t *s_game_ptr;
static int            s_game_len;

static cfg_msg_cb_t   s_msg_cb;
static void          *s_msg_user;
static cfg_conn_cb_t  s_conn_cb;
static void          *s_conn_user;
static cfg_conn_cb_t  s_pending_conn_cb;
static void          *s_pending_conn_user;
static portMUX_TYPE   s_conn_registration_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_event s_conn_registration_event;
static bool           s_conn_registration_ready;
static cfg_disconnect_cb_t s_disconnect_cb;
static void          *s_disconnect_user;
static frame_reasm_t  s_reasm;
static uint16_t       s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool           s_shutting_down;
static bool           s_host_synced;
static bool           s_stack_ready, s_voice_paused, s_voice_radio_fault;
static atomic_bool    s_voice_host_ready;
static uint16_t       s_notify_handle = 0;   // notify 特征 val handle(GATT 注册时回填)
static bool           s_subscribed = false;  // 客户端是否已订阅通知(写了 CCCD)
static uint16_t       s_capture_handle = 0;  // 截图 notify 特征 val handle(GATT 注册时回填)
static bool           s_capture_subscribed = false;
#define REASM_TIMEOUT_US  2000000            // 一帧内相邻写包间隔上限(2s)
static int64_t        s_last_write_us = 0;
static uint8_t        s_addr_type;
static struct ble_npl_event s_adv_restart_event;
static struct ble_npl_callout s_adv_retry_callout;
static bool s_adv_mode_change_pending;

static int gap_event(struct ble_gap_event *event, void *arg);   // 前置声明(start_adv 用)

// 身份读:返回 app 推入的 72 字节公开信息；v2 保留 Key 槽但始终置零。
static int ident_read(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (s_ident_ptr == NULL || s_ident_len <= 0) return 0;
    return os_mbuf_append(ctxt->om, s_ident_ptr, s_ident_len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// 游戏积分读:返回 app 推入的 blob([ver][game_total u32][game_best u32])。
static int game_read(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (s_game_ptr == NULL || s_game_len <= 0) return 0;
    return os_mbuf_append(ctxt->om, s_game_ptr, s_game_len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// notify 特征不可读写(值靠主动 notify 推送),但 NimBLE 要求每个特征 access_cb 非空
// (ble_gatts_chr_is_sane 检查,NULL 会使 ble_gatts_start 返回 EINVAL → host 启动 assert)。
// NOTIFY flag 下 att 层不含 read/write,客户端读会被 ATT 拒,此桩实际不会被调用。
static int notify_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

// 从 host task 发 notify [type][status]。未连接或无句柄则静默跳过。
static void cfg_notify(uint8_t type, uint8_t status) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "notify 跳过: 未连接 (type=0x%02X status=%d)", type, status);
        return;
    }
    if (s_notify_handle == 0) {
        ESP_LOGW(TAG, "notify 跳过: 通知特征句柄为0 (GATT 未注册?)");
        return;
    }
    if (!s_subscribed) {   // 仅告警,仍照发:避免订阅检测误判把正常通知拦掉
        ESP_LOGW(TAG, "客户端尚未订阅CCCD(仍尝试发送)。小程序需对 ...0011 特征调用 "
                      "notifyBLECharacteristicValueChange 订阅,否则收不到通知");
    }
    uint8_t pkt[2] = { type, status };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(pkt, sizeof pkt);
    if (!om) { ESP_LOGE(TAG, "notify: mbuf 分配失败"); return; }
    int rc = ble_gatts_notify_custom(s_conn, s_notify_handle, om);
    if (rc == 0) ESP_LOGD(TAG, "notify → type=0x%02X status=%d (conn=%d handle=%d)",  // 每片 ACK,降 DEBUG
                          type, status, s_conn, s_notify_handle);
    else         ESP_LOGE(TAG, "notify 发送失败 rc=%d (type=0x%02X status=%d)", rc, type, status);
}

// 截图分包走独立的 notify 特征。必须已连接且中央已订阅,避免把高频数据送入无消费者的链路。
static bool cfg_notify_capture(hal_config_t *self, const uint8_t *data, size_t len) {
    (void)self;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_capture_handle == 0 || !s_capture_subscribed) {
        return false;
    }
    if (data == NULL || len == 0) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGE(TAG, "capture notify: mbuf 分配失败");
        return false;
    }
    int rc = ble_gatts_notify_custom(s_conn, s_capture_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "capture notify 发送失败 rc=%d", rc);
        return false;
    }
    return true;
}

// 重组超时看门狗:小程序若只发了半帧就停,不会再有写入触发被动检测,
// 故用周期定时器主动发现并回报,避免小程序一直等不到回应。
static esp_timer_handle_t s_reasm_timer;
static void reasm_timeout_cb(void *arg) {
    (void)arg;
    if (s_reasm.have == 0) return;
    if (esp_timer_get_time() - s_last_write_us <= REASM_TIMEOUT_US) return;
    ESP_LOGW(TAG, "重组超时:丢弃半帧(%d 字节)并回报小程序", s_reasm.have);
    frame_reasm_reset(&s_reasm);
    cfg_notify(CFG_MSG_FRAME, CFG_ST_ERR_TIMEOUT);
}

// 命令写:累积字节 → 重组 → 完整帧交给上层消息回调。运行于 NimBLE host task。
static int cmd_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t seg[FRAME_HDR_LEN + FRAME_MAX_PAYLOAD];   // 容纳单次最大 ATT 写(可能整帧一次到)
    uint16_t got = 0;
    uint16_t avail = OS_MBUF_PKTLEN(ctxt->om);
    if (avail > sizeof seg) avail = sizeof seg;
    if (ble_hs_mbuf_to_flat(ctxt->om, seg, avail, &got) != 0) return BLE_ATT_ERR_UNLIKELY;

    int64_t now = esp_timer_get_time();
    if (s_reasm.have > 0 && (now - s_last_write_us) > REASM_TIMEOUT_US) {
        ESP_LOGW(TAG, "重组超时,丢弃半帧(已累计 %d 字节)并回报", s_reasm.have);
        frame_reasm_reset(&s_reasm);
        cfg_notify(CFG_MSG_FRAME, CFG_ST_ERR_TIMEOUT);
    }
    s_last_write_us = now;

    uint8_t type; const uint8_t *pl; int pl_len;
    ESP_LOGD(TAG, "收到写入 %d 字节(此前已累计 %d)", got, s_reasm.have);
    frame_status_t st = frame_reasm_push(&s_reasm, seg, got, &type, &pl, &pl_len);
    if (st == FRAME_READY) {
        // 音频流(0x04)每秒几十帧,逐帧 INFO 日志会占满串口并阻塞 host 任务拖慢收包 → 降为 DEBUG。
        // 批量分片(图片0x02/乐谱0x03/音频流0x04/音频片段0x05)每秒几十帧,逐帧 INFO 会刷屏
        // 并拖慢 host 任务 → 降 DEBUG;仅配置类(0x01)保留 INFO。结果由上层"显示/播放结果"给出。
        if (type == 0x01) ESP_LOGI(TAG, "◀ 完整帧: type=0x01 payload=%d 字节", pl_len);
        else              ESP_LOGD(TAG, "◀ 完整帧: type=0x%02X payload=%d 字节", type, pl_len);
        if (s_msg_cb) s_msg_cb(type, pl, pl_len, s_msg_user);
        else ESP_LOGW(TAG, "上层未注册消息回调,帧被丢弃");
    } else if (st == FRAME_ERR_VER || st == FRAME_ERR_LEN) {
        ESP_LOGE(TAG, "帧头非法: %s", st == FRAME_ERR_LEN ? "len 超过 256" : "ver 不是 0x01");
        cfg_notify(CFG_MSG_FRAME,
                   st == FRAME_ERR_LEN ? CFG_ST_ERR_TOO_LONG : CFG_ST_ERR_FRAME);
    }
    return 0;
}

static struct ble_gatt_chr_def g_chrs[6];    // cmd, notify, identity, gamescore, capture, 结束哨兵
static struct ble_gatt_svc_def g_svcs[2];

static void build_gatt_table(void) {
    // 0: 命令写
    g_chrs[0].uuid = &g_cmd_uuid.u;
    g_chrs[0].access_cb = cmd_write;
    g_chrs[0].arg = NULL;
    // 同时允许"带响应写"(配置/图片/乐谱,保证有序可靠)与"无响应写"(音频流,免每帧往返 ACK,
    // 吞吐从 ~2KB/s 提到满足 16k ADPCM 的 8KB/s)。两种写都进同一 access_cb,处理一致。
    g_chrs[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP;
    g_chrs[0].descriptors = NULL;
    // 1: notify 状态
    g_chrs[1].uuid = &g_notify_uuid.u;
    g_chrs[1].access_cb = notify_access;      // NimBLE 要求非空桩(NOTIFY 不会被读写)
    g_chrs[1].arg = NULL;
    g_chrs[1].flags = BLE_GATT_CHR_F_NOTIFY;
    g_chrs[1].val_handle = &s_notify_handle;  // 注册后回填 val handle
    g_chrs[1].descriptors = NULL;
    // 2: 身份只读
    g_chrs[2].uuid = &g_ident_uuid.u;
    g_chrs[2].access_cb = ident_read;
    g_chrs[2].arg = NULL;
    g_chrs[2].flags = BLE_GATT_CHR_F_READ;
    g_chrs[2].descriptors = NULL;
    // 3: 游戏积分只读
    g_chrs[3].uuid = &g_game_uuid.u;
    g_chrs[3].access_cb = game_read;
    g_chrs[3].arg = NULL;
    g_chrs[3].flags = BLE_GATT_CHR_F_READ;
    g_chrs[3].descriptors = NULL;
    // 4: 屏幕截图 notify(独立 CCCD,不影响旧客户端的状态通知订阅)
    g_chrs[4].uuid = &g_capture_uuid.u;
    g_chrs[4].access_cb = notify_access;
    g_chrs[4].arg = NULL;
    g_chrs[4].flags = BLE_GATT_CHR_F_NOTIFY;
    g_chrs[4].val_handle = &s_capture_handle;
    g_chrs[4].descriptors = NULL;
    memset(&g_chrs[5], 0, sizeof g_chrs[5]);  // 结束哨兵

    g_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svcs[0].uuid = &g_svc_uuid.u;
    g_svcs[0].characteristics = g_chrs;
    memset(&g_svcs[1], 0, sizeof g_svcs[1]);
}

static void schedule_adv_retry(void) {
    if (!s_host_synced || s_shutting_down) return;
    /* Controller/resource failures can be transient. Stay on the user's chosen
     * screen and recover without a busy loop or requiring another mode entry. */
    (void)ble_npl_callout_reset(&s_adv_retry_callout, ble_npl_time_ms_to_ticks32(1000));
}

static void start_adv(void) {
    if (!s_host_synced || s_shutting_down || s_conn != BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_adv_active() || ble_clicker_pairing_paused_host()) return;
    struct ble_gap_adv_params advp;
    struct ble_hs_adv_fields f;
    memset(&advp, 0, sizeof(advp));
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const bool clicker = ble_clicker_advertising_active();
    const char *name = clicker ? ble_clicker_name() : s_dev_name;
    static const ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    f.name = (uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    if (clicker) {
        f.uuids16 = (ble_uuid16_t *)&hid_uuid;
        f.num_uuids16 = 1;
        f.uuids16_is_complete = 1;
        f.appearance = 0x03c0;  // Generic HID: keyboard and mouse
        f.appearance_is_present = 1;
    }
    /* NimBLE cannot replace an IRK/bond while observer scanning is active.
     * Do this before advertising so a host can never pair during that window. */
    int rc = ble_clicker_prepare_services_host(clicker);
    if (rc == 0) rc = ble_scan_set_enabled(s_addr_type, !clicker);
    if (rc == 0 && clicker) rc = ble_clicker_prune_orphan_subscriptions();
    if (rc == 0) rc = ble_svc_gap_device_name_set(name);
    if (rc == 0) rc = ble_svc_gap_device_appearance_set(clicker ? 0x03c0 : 0);
    if (rc == 0) rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "%s advertising fields failed rc=%d", clicker ? "clicker" : "standard", rc);
        ble_clicker_advertising_result_host(false, rc);
        schedule_adv_retry();
        return;
    }
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &advp, gap_event, NULL);
    ble_clicker_advertising_result_host(clicker && rc == 0, rc);
    if (rc != 0) {
        ESP_LOGW(TAG, "%s advertising start failed rc=%d", clicker ? "clicker" : "standard", rc);
        schedule_adv_retry();
    } else {
        ble_npl_callout_stop(&s_adv_retry_callout);
        ESP_LOGI(TAG, "%s advertising as %s", clicker ? "clicker" : "standard", name);
    }
}

static void adv_restart_cb(struct ble_npl_event *event) {
    (void)event;
    if (!s_host_synced || s_shutting_down) return;
    if (s_adv_mode_change_pending) ble_config_clicker_mode_changed_host();
    else start_adv();
}

static void queue_adv_restart(void) {
    if (!s_host_synced || s_shutting_down || ble_npl_event_is_queued(&s_adv_restart_event)) return;
    ble_npl_callout_stop(&s_adv_retry_callout);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_adv_restart_event);
}

void ble_config_clicker_mode_changed_host(void) {
    if (s_shutting_down) return;
    s_adv_mode_change_pending = true;
    ble_clicker_advertising_result_host(false, 0);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        int rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "mode switch disconnect failed rc=%d conn=%d", rc, s_conn);
            ble_clicker_advertising_result_host(false, rc);
            schedule_adv_retry();
        }
        return;
    }
    if (ble_gap_adv_active()) {
        int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "mode switch advertising stop failed rc=%d", rc);
            ble_clicker_advertising_result_host(false, rc);
            schedule_adv_retry();
            return;
        }
    }
    s_adv_mode_change_pending = false;
    /* NimBLE requires a restart after adv_stop() to run in a later event. */
    queue_adv_restart();
}

// ATT Exchange MTU 每条连接只发起一次，实际 MTU 取双方支持值的最小值。
// NimBLE 在首次请求发出后锁定本连接的本地 MTU；回调失败时再改全局 preferred
// 既不能为该连接“降档”，还会影响下次连接。上限 256 与 msys 缓冲配置一致。
#define CONFIG_PREFERRED_MTU 256
static uint32_t s_mtu_generation;

static bool config_connection_ready(uint16_t conn) {
    return conn != BLE_HS_CONN_HANDLE_NONE && conn == s_conn &&
           !s_shutting_down && !s_adv_mode_change_pending &&
           !ble_clicker_connection_closing(conn);
}

static int mtu_exch_cb(uint16_t conn, const struct ble_gatt_error *error, uint16_t mtu, void *arg) {
    // NimBLE tears down GATT before delivering GAP_DISCONNECT. s_conn alone
    // still looks live in that window; ENOTCONN is cleanup, not a negotiation error.
    if (error->status == BLE_HS_ENOTCONN) {
        ESP_LOGD(TAG, "连接已结束,停止 MTU 协商 conn=%d", conn);
        return 0;
    }
    if ((uint32_t)(uintptr_t)arg != s_mtu_generation || !config_connection_ready(conn)) return 0;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return 0;
    if (error->status == 0) {
        ESP_LOGI(TAG, "主动 MTU 协商成功 = %d(请求档 %d,单次写上限约 %d 字节)",
                 mtu, CONFIG_PREFERRED_MTU, mtu - 3);
        return 0;
    }
    if (error->status == BLE_HS_EALREADY) {            // 本端已经发起过交换 → 读取现值
        ESP_LOGI(TAG, "MTU 已发起过协商,当前 = %d", ble_att_mtu(conn));
        return 0;
    }
    ESP_LOGW(TAG, "MTU 协商未完成(status=%d),沿用当前 %d", error->status, ble_att_mtu(conn));
    return 0;
}

static void start_mtu_negotiation(uint16_t conn) {
    if (!config_connection_ready(conn)) return;
    if (++s_mtu_generation == 0) ++s_mtu_generation;
    ble_att_set_preferred_mtu(CONFIG_PREFERRED_MTU);
    int rc = ble_gattc_exchange_mtu(conn, mtu_exch_cb, (void *)(uintptr_t)s_mtu_generation);
    if (rc != 0 && rc != BLE_HS_ENOTCONN)
        ESP_LOGI(TAG, "MTU 未发起主动协商(rc=%d),当前 %d", rc, ble_att_mtu(conn));
}

// Capture parameter policy: host-event-owned (no new task or allocated queue).
// The capture worker owns modem-sleep balancing; only this default host event
// touches GAP parameters. A generation, not a reusable connection handle, owns
// the saved parameters. Central rejection is a best-effort throughput downgrade.
static struct ble_npl_event s_capture_params_event;
static StaticSemaphore_t s_capture_publish_storage;
static SemaphoreHandle_t s_capture_publish_mutex; // initialized once, never deleted on radio restart
static atomic_bool s_capture_params_ready, s_capture_link_live, s_capture_params_hold;
static atomic_uint s_capture_link_generation, s_capture_request_generation, s_capture_request_id;
static bool s_capture_modem_held; // serialized capture worker; retained on enable failure
static struct {
    bool valid, low_attempted, restore_attempted, restore_wait, restore_defer_used;
    unsigned pending; // 0:none, 1:low-latency request, 2:restore request
    uint16_t conn;
    unsigned generation, request_id;
    struct ble_gap_upd_params original;
} s_capture_params;

static void capture_params_queue(void) {
    // NPL put can block on its queue, so use a task mutex, never a portMUX or
    // host lock. Shutdown closes ready under this same publication barrier.
    if (!s_capture_publish_mutex ||
        xSemaphoreTake(s_capture_publish_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "capture connection request not queued: publisher busy");
        return;
    }
    if (atomic_load(&s_capture_params_ready))
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_capture_params_event);
    xSemaphoreGive(s_capture_publish_mutex);
}

static bool capture_params_close_for_stop(void) {
    if (s_capture_publish_mutex &&
        xSemaphoreTake(s_capture_publish_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "capture publisher busy; BLE shutdown cancelled");
        return false;
    }
    atomic_store(&s_capture_params_ready, false);
    atomic_store(&s_capture_link_live, false);
    atomic_store(&s_capture_params_hold, false);
    if (s_capture_publish_mutex) xSemaphoreGive(s_capture_publish_mutex);
    return true;
}

static void capture_params_connection_changed_host(void) {
    atomic_fetch_add(&s_capture_link_generation, 1);
    atomic_store(&s_capture_link_live, config_connection_ready(s_conn));
    atomic_store(&s_capture_params_hold, false);
    memset(&s_capture_params, 0, sizeof s_capture_params);
}

static void capture_params_event_host(struct ble_npl_event *event) {
    (void)event;
    if (!atomic_load(&s_capture_params_ready)) return;
    unsigned generation = atomic_load(&s_capture_link_generation);
    unsigned requested = atomic_load(&s_capture_request_generation);
    bool hold = atomic_load(&s_capture_params_hold) && requested == generation;
    if (!atomic_load(&s_capture_link_live) || !config_connection_ready(s_conn) ||
        (s_capture_params.valid && s_capture_params.generation != generation)) {
        memset(&s_capture_params, 0, sizeof s_capture_params);
        return;
    }
    struct ble_gap_conn_desc d;
    if (ble_gap_conn_find(s_conn, &d) != 0) return;
    if (!s_capture_params.valid) {
        if (!hold) return;
        s_capture_params.valid = true;
        s_capture_params.conn = s_conn;
        s_capture_params.generation = generation;
        s_capture_params.original = (struct ble_gap_upd_params) {
            .itvl_min = d.conn_itvl, .itvl_max = d.conn_itvl,
            .latency = d.conn_latency, .supervision_timeout = d.supervision_timeout,
        };
    }
    if (hold && s_capture_params.request_id != atomic_load(&s_capture_request_id)) {
        s_capture_params.request_id = atomic_load(&s_capture_request_id);
        s_capture_params.low_attempted = s_capture_params.restore_attempted = false;
        s_capture_params.restore_wait = s_capture_params.restore_defer_used = false;
    }
    // Release may precede completion of the LL/L2CAP update. Wait for the GAP
    // event before restoring; never busy-poll or race another update procedure.
    if (s_capture_params.pending) return;
    if (hold) {
        if (s_capture_params.low_attempted) return;
        if (d.conn_latency == 0 && d.conn_itvl >= 12 && d.conn_itvl <= 24) return;
        // Initial connection parameters may already be fast; the central can
        // raise latency later. Spend our one attempt only when actually sent.
        s_capture_params.low_attempted = true;
        const struct ble_gap_upd_params low = {
            .itvl_min = 12, .itvl_max = 24, .latency = 0, .supervision_timeout = 400,
        };
        int rc = ble_gap_update_params(s_capture_params.conn, &low);
        if (rc == 0) s_capture_params.pending = 1;
        ESP_LOGI(TAG, "capture connection request rc=%d; current interval=%u latency=%u (not negotiated success)",
                 rc, d.conn_itvl, d.conn_latency);
    } else {
        const struct ble_gap_upd_params *p = &s_capture_params.original;
        if (d.conn_itvl == p->itvl_min && d.conn_latency == p->latency &&
            d.supervision_timeout == p->supervision_timeout) {
            memset(&s_capture_params, 0, sizeof s_capture_params);
            return;
        }
        if (s_capture_params.restore_attempted) return;
        s_capture_params.restore_attempted = true;
        int rc = ble_gap_update_params(s_capture_params.conn, p);
        if (rc == 0) s_capture_params.pending = 2;
        else if (rc == BLE_HS_EALREADY && !s_capture_params.restore_defer_used) {
            s_capture_params.restore_wait = s_capture_params.restore_defer_used = true;
        }
        ESP_LOGI(TAG, "capture connection restore request rc=%d; current interval=%u latency=%u",
                 rc, d.conn_itvl, d.conn_latency);
    }
}

static void capture_params_updated_host(uint16_t conn, int status) {
    if (!s_capture_params.valid || conn != s_capture_params.conn ||
        s_capture_params.generation != atomic_load(&s_capture_link_generation)) return;
    // The existing GAP logger reports the actual descriptor, including a peer
    // rejecting or selecting different parameters. Do not re-request while held.
    s_capture_params.pending = 0;
    if (s_capture_params.restore_wait) {
        s_capture_params.restore_wait = false;
        s_capture_params.restore_attempted = false; // one event-driven retry only
    }
    if (status != 0) ESP_LOGW(TAG, "capture connection negotiation status=%d", status);
    capture_params_event_host(NULL);
}
// End capture parameter policy

static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    int clicker_rc = ble_clicker_gap_event(event);
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) return clicker_rc;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                // 本传输端口仅服务一个中央。拒绝额外连接可使截图 CCCD 和 MTU
                // 始终对应同一 conn_handle,不会把 A 的订阅泄漏给 B。
                if (s_conn != BLE_HS_CONN_HANDLE_NONE && s_conn != event->connect.conn_handle) {
                    ESP_LOGW(TAG, "拒绝额外连接 conn=%d (活动连接=%d)",
                             event->connect.conn_handle, s_conn);
                    if (!ble_clicker_connection_closing(event->connect.conn_handle)) {
                        ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                    }
                    break;
                }
                s_conn = event->connect.conn_handle;
                ++s_mtu_generation; // Invalidate callbacks if NimBLE reuses a handle.
                capture_params_connection_changed_host();
                if (!config_connection_ready(s_conn)) {
                    ESP_LOGD(TAG, "连接正在退出,跳过参数/MTU/业务初始化 conn=%d", s_conn);
                    break;
                }
                ESP_LOGI(TAG, "已连接 conn=%d, MTU=%d(此刻多为默认23,协商后见 MTU 事件)", s_conn, ble_att_mtu(s_conn));
                // 请求更短的连接间隔:音频流需 8KB/s,默认间隔(常 ~30-50ms)只够 ~4KB/s → 卡顿。
                // 请求 15-30ms、latency=0,让每秒连接事件更多,吞吐翻倍以上。中心可能协商到别的值。
                struct ble_gap_upd_params up = {
                    .itvl_min = 12,   // 12 * 1.25ms = 15ms
                    .itvl_max = 24,   // 24 * 1.25ms = 30ms
                    .latency = 0,
                    .supervision_timeout = 400,   // 400 * 10ms = 4s
                };
                ble_gap_update_params(s_conn, &up);
                start_mtu_negotiation(s_conn);
                if (s_conn_cb) s_conn_cb(s_conn_user);   // 通知上层:已连接(播提示片段等)
            } else {
                ESP_LOGW(TAG, "连接失败 status=%d", event->connect.status);
                /* Failed establishment ends connectable advertising without a
                 * DISCONNECT event. Queue the same restart as a normal drop. */
                ble_clicker_advertising_result_host(false, 0);
                queue_adv_restart();
            }
            break;
        case BLE_GAP_EVENT_CONN_UPDATE: {
            if (event->conn_update.conn_handle != s_conn) break;
            capture_params_updated_host(event->conn_update.conn_handle, event->conn_update.status);
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s_conn, &d) == 0)
                ESP_LOGI(TAG, "连接参数更新 status=%d,间隔=%.2fms latency=%d timeout=%dms",
                         event->conn_update.status, d.conn_itvl * 1.25,
                         d.conn_latency, d.supervision_timeout * 10);
            else
                ESP_LOGI(TAG, "连接参数更新 status=%d", event->conn_update.status);
            break;
        }
        case BLE_GAP_EVENT_DISCONNECT:
            if (event->disconnect.conn.conn_handle != s_conn) {
                ESP_LOGD(TAG, "忽略非活动连接断开 conn=%d", event->disconnect.conn.conn_handle);
                break;
            }
            ESP_LOGI(TAG, "已断开 reason=%d, 重新广播", event->disconnect.reason);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            ++s_mtu_generation;
            capture_params_connection_changed_host();
            s_adv_mode_change_pending = false;
            s_subscribed = false;
            s_capture_subscribed = false;
            frame_reasm_reset(&s_reasm);
            if (s_disconnect_cb) s_disconnect_cb(s_disconnect_user);
            if (!s_shutting_down) queue_adv_restart();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            // 客户端写 CCCD 订阅/退订。只有订阅了 notify,卡片发的通知才收得到。
            ESP_LOGI(TAG, "订阅事件: attr=%d notify=%d indicate=%d (通知特征句柄=%d)",
                     event->subscribe.attr_handle, event->subscribe.cur_notify,
                     event->subscribe.cur_indicate, s_notify_handle);
            if (event->subscribe.attr_handle == s_notify_handle) {
                s_subscribed = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "通知特征订阅状态 → %s", s_subscribed ? "已订阅✓" : "未订阅✗");
            } else if (event->subscribe.attr_handle == s_capture_handle) {
                s_capture_subscribed = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "截图特征订阅状态 → %s", s_capture_subscribed ? "已订阅✓" : "未订阅✗");
            }
            break;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU 协商 = %d (单次写上限约 %d 字节)",
                     event->mtu.value, event->mtu.value - 3);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ble_clicker_advertising_result_host(false, 0);
            if (!s_shutting_down) queue_adv_restart();
            break;
        default: break;
    }
    return 0;
}

static void on_sync(void) {
    ble_hs_id_infer_auto(0, &s_addr_type);
    s_host_synced = true;
    ble_clicker_host_synced();
    start_adv();
    // Publish only after controller bring-up and our sync callback finish.
    atomic_store(&s_voice_host_ready, true);
}
static void on_reset(int reason) {
    atomic_store(&s_voice_host_ready, false);
    s_host_synced = false;
    ble_npl_callout_stop(&s_adv_retry_callout);
    s_adv_mode_change_pending = false;
    /* A host reset can destroy a link without the normal GAP disconnect path. */
    const bool had_connection = s_conn != BLE_HS_CONN_HANDLE_NONE;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    ++s_mtu_generation;
    capture_params_connection_changed_host();
    s_subscribed = false;
    s_capture_subscribed = false;
    frame_reasm_reset(&s_reasm);
    ble_clicker_host_reset();
    if (had_connection && s_disconnect_cb) s_disconnect_cb(s_disconnect_user);
    ESP_LOGW(TAG, "nimble reset, reason=%d", reason);
}
static void host_task(void *param) {
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void cfg_on_message(hal_config_t *self, cfg_msg_cb_t cb, void *user) {
    (void)self; s_msg_cb = cb; s_msg_user = user;
}
static void on_conn_registration(struct ble_npl_event *event) {
    (void)event;
    portENTER_CRITICAL(&s_conn_registration_lock);
    cfg_conn_cb_t cb = s_pending_conn_cb;
    void *user = s_pending_conn_user;
    portEXIT_CRITICAL(&s_conn_registration_lock);

    bool changed = s_conn_cb != cb || s_conn_user != user;
    s_conn_user = user;
    s_conn_cb = cb;
    if (changed && cb && config_connection_ready(s_conn)) {
        cb(user);
    }
}

static void cfg_on_connect(hal_config_t *self, cfg_conn_cb_t cb, void *user) {
    (void)self;
    if (!s_conn_registration_ready) return;   // NimBLE 初始化失败时没有可用事件队列
    portENTER_CRITICAL(&s_conn_registration_lock);
    s_pending_conn_user = user;
    s_pending_conn_cb = cb;
    portEXIT_CRITICAL(&s_conn_registration_lock);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_conn_registration_event);
}
static void cfg_on_disconnect(hal_config_t *self, cfg_disconnect_cb_t cb, void *user) {
    (void)self; s_disconnect_user = user; s_disconnect_cb = cb;
}
static void cfg_notify_status(hal_config_t *self, uint8_t type, uint8_t status) {
    (void)self; cfg_notify(type, status);
}
static bool cfg_capture_subscribed(hal_config_t *self) {
    (void)self;
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_capture_subscribed;
}
static uint16_t cfg_get_mtu_payload(hal_config_t *self) {
    (void)self;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return 20;
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu <= 3) return 20;
    uint16_t payload = (uint16_t)(mtu - 3);
    return payload > 244 ? 244 : payload;
}
static bool cfg_capture_transport_hold(hal_config_t *self, bool hold) {
    (void)self;
    unsigned generation = atomic_load(&s_capture_link_generation);
    if (hold && (!atomic_load(&s_capture_params_ready) ||
                 !atomic_load(&s_capture_link_live))) return false;
    if (hold && s_capture_modem_held && atomic_load(&s_capture_params_hold) &&
        atomic_load(&s_capture_request_generation) == generation) return true;
    esp_err_t err = ESP_OK;
    if (hold && !s_capture_modem_held) {
        err = esp_bt_sleep_disable();
        if (err == ESP_OK) s_capture_modem_held = true;
    } else if (!hold && s_capture_modem_held) {
        err = esp_bt_sleep_enable();
        if (err == ESP_OK) s_capture_modem_held = false;
    }
    if (!hold) atomic_store(&s_capture_params_hold, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "截图传输%s BLE modem sleep 失败: %s",
                 hold ? "暂停" : "恢复", esp_err_to_name(err));
        capture_params_queue(); // release still restores GAP after enable failure
        return false;
    }
    if (hold) {
        if (generation != atomic_load(&s_capture_link_generation) ||
            !atomic_load(&s_capture_link_live)) {
            (void)cfg_capture_transport_hold(self, false);
            return false;
        }
        atomic_store(&s_capture_request_generation, generation);
        atomic_fetch_add(&s_capture_request_id, 1);
        atomic_store(&s_capture_params_hold, true);
    }
    capture_params_queue();
    ESP_LOGI(TAG, "截图传输 BLE modem sleep → %s", hold ? "暂停" : "恢复");
    return true;
}
static int cfg_shutdown(hal_config_t *self) {
    (void)self;
    if (!s_stack_ready) return 0;
    if (!capture_params_close_for_stop()) return -1;
    // 深睡会关闭无线域；这里先结束 GAP 活动，让手机收到正常断开并避免临睡前继续收包。
    s_shutting_down = true;
    ble_npl_callout_stop(&s_adv_retry_callout);
    ble_clicker_begin_shutdown();
    (void)ble_gap_disc_cancel();
    (void)ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    (void)esp_bt_sleep_enable();
    ESP_LOGI(TAG, "BLE 广播/扫描已停止，活动连接已请求断开");
    return 0;
}
static void cfg_set_identity(hal_config_t *self, const uint8_t *data, int len) {
    (void)self; s_ident_ptr = data; s_ident_len = len;
}
static void cfg_set_gamescore(hal_config_t *self, const uint8_t *data, int len) {
    (void)self; s_game_ptr = data; s_game_len = len;
}
static const hal_config_api_t API = {
    .on_message = cfg_on_message,
    .on_connect = cfg_on_connect,
    .on_disconnect = cfg_on_disconnect,
    .notify_status = cfg_notify_status,
    .notify_capture = cfg_notify_capture,
    .capture_subscribed = cfg_capture_subscribed,
    .get_mtu_payload = cfg_get_mtu_payload,
    .capture_transport_hold = cfg_capture_transport_hold,
    .shutdown = cfg_shutdown,
    .set_identity = cfg_set_identity,
    .set_gamescore = cfg_set_gamescore,
};
static hal_config_t s_handle = { .api = &API, .impl = NULL };

hal_config_t *platform_create_ble_config(const board_config_t *cfg, const char *dev_name) {
    atomic_store(&s_capture_params_ready, false);
    if (!s_capture_publish_mutex)
        s_capture_publish_mutex = xSemaphoreCreateMutexStatic(&s_capture_publish_storage);
    atomic_store(&s_voice_host_ready, false);
    s_shutting_down = false;
    s_conn_registration_ready = false;
    // 名字为空时回落到编译期常量,保证任何情况下都能被搜到
    if (dev_name && dev_name[0]) {
        strncpy(s_dev_name, dev_name, sizeof s_dev_name - 1);
        s_dev_name[sizeof s_dev_name - 1] = '\0';
    }
    (void)cfg;
    // BLE/PHY 和 HID bond 共用默认 NVS。KV 在本函数之前初始化该分区；这里绝不
    // 自动擦除，避免一次 BLE 初始化错误连带清空用户 profile、宠物存档和 bond。
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (0x%x); refusing destructive erase, BLE disabled", nvs_err);
        return &s_handle;
    }
    // 防御:host 初始化可能因堆不足失败(如 esp_nimble_hci_init 的 ble_buf_alloc
    // 返回 NO_MEM)。此时必须提前返回,否则后面的 ble_gatts_count_cfg 会在未初始化的
    // host 上解引用空指针 → Load access fault。返回句柄让上层照常注册回调(不会被调用)。
    esp_err_t ble_err = nimble_port_init();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed (0x%x); BLE disabled", ble_err);
        return &s_handle;
    }
    s_stack_ready = true;
    s_capture_modem_held = false;
    atomic_store(&s_capture_params_hold, false);
    memset(&s_capture_params, 0, sizeof s_capture_params);
    ble_npl_event_init(&s_capture_params_event, capture_params_event_host, NULL);
    atomic_store(&s_capture_params_ready, true);
    ble_npl_event_init(&s_conn_registration_event, on_conn_registration, NULL);
    s_conn_registration_ready = true;
    frame_reasm_reset(&s_reasm);
    {   // 重组超时看门狗:周期检查,发现半帧超时则丢弃并主动回报小程序
        const esp_timer_create_args_t targs = { .callback = reasm_timeout_cb, .name = "reasm_to" };
        if (esp_timer_create(&targs, &s_reasm_timer) == ESP_OK)
            esp_timer_start_periodic(s_reasm_timer, 500000);   // 每 500ms 检查
    }
    // NimBLE 主机每次 notify/GAP 动作都打 INFO("GATT procedure initiated: notify; att_handle=..")
    // —— 图片传输时每片一条,严重刷屏。压到 WARN,只留告警/错误;我方 ble_config 日志不受影响。
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    build_gatt_table();
    int rc = ble_gatts_count_cfg(g_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(g_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "configuration GATT registration failed rc=%d", rc);
    }
    if (ble_clicker_init() != 0) {
        ESP_LOGE(TAG, "clicker unavailable; standard BLE remains enabled");
    }
    ble_npl_event_init(&s_adv_restart_event, adv_restart_cb, NULL);
    ble_npl_callout_init(&s_adv_retry_callout, nimble_port_get_dflt_eventq(),
                         adv_restart_cb, NULL);
    ble_svc_gap_device_name_set(s_dev_name);
    nimble_port_freertos_init(host_task);
    return &s_handle;
}
static bool wait_voice_host_ready(void) {
    // nimble_port_freertos_init starts asynchronously. Stopping during HCI
    // bring-up can deinitialize ble_hs_timer before ble_hs_sync uses it.
    // Called only by the plays worker outside the LVGL lock.
    for (unsigned i=0; i<200 && !atomic_load(&s_voice_host_ready); i++)
        vTaskDelay(pdMS_TO_TICKS(10));
    return atomic_load(&s_voice_host_ready);
}
bool platform_ble_voice_pause(void) {
    // A partial shutdown has unknown controller ownership. Fail closed until
    // reboot instead of treating it as an absent (never initialized) BLE host.
    if (s_voice_radio_fault) return false;
    if (s_voice_paused || !s_stack_ready) return true;
    if (!wait_voice_host_ready()) {
        ESP_LOGE(TAG, "BLE host not ready for voice shutdown; restart device");
        s_voice_radio_fault = true;
        return false;
    }
    s_conn_registration_ready = false;
    if (cfg_shutdown(&s_handle) != 0) {
        ESP_LOGE(TAG, "Cannot close BLE publishers for voice");
        s_voice_radio_fault = true;
        return false;
    }
    if (nimble_port_stop() != 0) {
        ESP_LOGE(TAG, "Cannot stop BLE host for voice");
        s_voice_radio_fault = true;
        return false;
    }
    atomic_store(&s_voice_host_ready, false);
    atomic_store(&s_capture_params_ready, false);
    // Host callbacks have stopped. Clear every external timer/callout before
    // releasing NimBLE's dynamic pools; these objects are rebuilt on return.
    if (s_reasm_timer) {
        esp_timer_stop(s_reasm_timer); esp_timer_delete(s_reasm_timer); s_reasm_timer = NULL;
    }
    ble_clicker_deinit_after_host_stop();
    ble_npl_event_deinit(&s_conn_registration_event);
    ble_npl_event_deinit(&s_capture_params_event);
    ble_npl_event_deinit(&s_adv_restart_event);
    ble_npl_callout_deinit(&s_adv_retry_callout);
    s_host_synced = false;
    const bool connected = s_conn != BLE_HS_CONN_HANDLE_NONE;
    s_conn = BLE_HS_CONN_HANDLE_NONE; s_subscribed = s_capture_subscribed = false;
    capture_params_connection_changed_host(); // host stopped; queued old work is gone
    ++s_mtu_generation; frame_reasm_reset(&s_reasm);
    if (connected && s_disconnect_cb) s_disconnect_cb(s_disconnect_user);
    s_stack_ready = false;
    if (nimble_port_deinit() != ESP_OK) {
        ESP_LOGE(TAG, "BLE deinitialization failed; restart device before voice");
        s_voice_radio_fault = true;
        return false;
    }
    s_voice_paused = true;
    return true;
}
bool platform_ble_voice_resume(void) {
    if (s_voice_radio_fault) return false;
    if (!s_voice_paused) return true;
    platform_create_ble_config(NULL, NULL);
    if (!s_stack_ready || !wait_voice_host_ready()) {
        s_voice_radio_fault = true; return false;
    }
    s_voice_paused = false;
    return true;
}
#else
bool platform_ble_voice_pause(void) { return true; }
bool platform_ble_voice_resume(void) { return true; }

#endif // PERIPH_BLE
