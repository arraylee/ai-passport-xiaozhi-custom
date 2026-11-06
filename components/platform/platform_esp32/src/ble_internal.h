// components/platform/platform_esp32/src/ble_internal.h —— 平台内 BLE 协作接口
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct ble_gap_event;

// 由 ble_config.c 的 on_sync 调用:在广播启动后启动 BLE 扫描(observer)。
// addr_type 为 ble_hs_id_infer_auto 推得的本机地址类型。
int ble_scan_set_enabled(uint8_t addr_type, bool enabled);
int ble_clicker_prune_orphan_subscriptions(void);

/* Clicker hooks. Initialization happens before the host starts; every runtime
 * BLE mutation is dispatched on the NimBLE host event queue. */
int ble_clicker_init(void);
int ble_clicker_device_info_init(void);
/* Before a new host/database is initialized (not on a controller reset). */
void ble_clicker_services_reset(void);
/* Host task only, with no connection or advertising: expose secured HOGP
 * services only in Clicker mode. A failure must prevent advertising. */
int ble_clicker_prepare_services_host(bool active);
void ble_clicker_host_synced(void);
void ble_clicker_host_reset(void);
bool ble_clicker_advertising_active(void);
/* Hold advertising until the requested HID peer removal has completed. */
bool ble_clicker_pairing_paused_host(void);
/* Reports the controller's actual advertising state, or a transport error.
 * Called on the host task; mode intent alone is not proof of discoverability. */
void ble_clicker_advertising_result_host(bool active, int error);
const char *ble_clicker_name(void);
int ble_clicker_gap_event(struct ble_gap_event *event);
/* Host task only: an explicit exit / rejected connection is awaiting GAP drop.
 * Do not start configuration or MTU work on that connection. */
bool ble_clicker_connection_closing(uint16_t conn);
void ble_clicker_begin_shutdown(void);
void ble_clicker_deinit_after_host_stop(void);

/* Called by the clicker adapter from NimBLE host context. */
void ble_config_clicker_mode_changed_host(void);
