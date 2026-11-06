#include "platform/board_config.h"
#if PERIPH_BLE
#include "ble_internal.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include <string.h>

/* Only subscription metadata is pruned. Never remove security keys here.
 * Older builds could fail to replace a bond while observer scanning was active,
 * then persist CCCDs for the unpersisted peer and fill the five-entry table. */
int ble_clicker_prune_orphan_subscriptions(void)
{
    if (ble_gap_disc_active() || ble_gap_adv_active()) return BLE_HS_EBUSY;
    const int types[] = {BLE_STORE_OBJ_TYPE_CCCD, BLE_STORE_OBJ_TYPE_CSFC};
    unsigned removed = 0;
    for (unsigned t = 0; t < sizeof types / sizeof types[0]; ++t) {
        unsigned index = 0;
        while (index < 255) {
            union ble_store_key key = {0};
            union ble_store_value value = {0};
            if (types[t] == BLE_STORE_OBJ_TYPE_CCCD) {
                key.cccd.peer_addr = *BLE_ADDR_ANY;
                key.cccd.idx = index;
            } else {
                key.csfc.peer_addr = *BLE_ADDR_ANY;
                key.csfc.idx = index;
            }
            int rc = ble_store_read(types[t], &key, &value);
            if (rc == BLE_HS_ENOENT) break;
            if (rc != 0) return rc;
            ble_addr_t peer = types[t] == BLE_STORE_OBJ_TYPE_CCCD
                                 ? value.cccd.peer_addr : value.csfc.peer_addr;
            union ble_store_key sec_key = {0};
            union ble_store_value sec = {0};
            sec_key.sec.peer_addr = peer;
            int ours = ble_store_read(BLE_STORE_OBJ_TYPE_OUR_SEC, &sec_key, &sec);
            int theirs = ble_store_read(BLE_STORE_OBJ_TYPE_PEER_SEC, &sec_key, &sec);
            memset(&sec, 0, sizeof sec);
            if (ours != 0 && ours != BLE_HS_ENOENT) return ours;
            if (theirs != 0 && theirs != BLE_HS_ENOENT) return theirs;
            if (ours == 0 || theirs == 0) {
                ++index;
                continue;
            }
            memset(&key, 0, sizeof key);
            if (types[t] == BLE_STORE_OBJ_TYPE_CCCD) {
                key.cccd.peer_addr = peer;
                key.cccd.chr_val_handle = value.cccd.chr_val_handle;
            } else {
                key.csfc.peer_addr = peer;
            }
            rc = ble_store_delete(types[t], &key);
            if (rc != 0) return rc;
            ++removed; /* Deletion shifts the next entry into the same index. */
        }
    }
    if (removed) ESP_LOGI("clicker_store", "Removed %u orphan subscriptions; bonds preserved", removed);
    return 0;
}
#endif
