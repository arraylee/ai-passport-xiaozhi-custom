#include "platform/board_config.h"
#include "platform/platform_clicker.h"

#if PERIPH_BLE
#include "ble_internal.h"
#include "esp_log.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "services/gatt/ble_svc_gatt.h"
#include <stdatomic.h>

/* HOGP requires DIS/PnP and BAS alongside HIDS (HOGP section 3).
 * DIS remains public in standard mode. HIDS and BAS are fully registered but
 * hidden until Clicker is active, so OS discovery cannot initiate an unrelated
 * HID security procedure on a configuration link. No secret is exposed.
 * 0xffff is the Bluetooth internal-test vendor ID, NOT a FoloToy allocation.
 * Replace with an assigned company ID before public production distribution.
 * BAS is read-only (BAS 3.1 permits this), so no extra CCCD/notify queue is needed.
 */
static const ble_uuid16_t s_dis_uuid = BLE_UUID16_INIT(0x180a);
static const ble_uuid16_t s_pnp_uuid = BLE_UUID16_INIT(0x2a50);
static const ble_uuid16_t s_manufacturer_uuid = BLE_UUID16_INIT(0x2a29);
static const ble_uuid16_t s_model_uuid = BLE_UUID16_INIT(0x2a24);
static const ble_uuid16_t s_bas_uuid = BLE_UUID16_INIT(0x180f);
static const ble_uuid16_t s_battery_uuid = BLE_UUID16_INIT(0x2a19);
static const ble_uuid16_t s_hids_uuid = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t s_hid_last_uuid = BLE_UUID16_INIT(0x2a4c);
static const uint8_t s_pnp_id[] = {0x01, 0xff, 0xff, 0x01, 0x00, 0x30, 0x01};
static atomic_int s_battery_level = -1;

static struct {
    const ble_uuid_t *uuid;
    const ble_uuid_t *last_chr_uuid;
    uint16_t start, end;
    bool visible;
} s_secure_services[] = {
    { .uuid = &s_hids_uuid.u, .last_chr_uuid = &s_hid_last_uuid.u, .visible = true },
    { .uuid = &s_bas_uuid.u, .last_chr_uuid = &s_battery_uuid.u, .visible = true },
};

void ble_clicker_services_reset(void)
{
    for (size_t i = 0; i < sizeof s_secure_services / sizeof s_secure_services[0]; ++i) {
        s_secure_services[i].start = s_secure_services[i].end = 0;
        s_secure_services[i].visible = true; // New ATT database registers visible.
    }
}

int ble_clicker_prepare_services_host(bool active)
{
    uint16_t first = UINT16_MAX, last = 0;
    int rc;
    /* Resolve both ranges before moving any ATT entries. Missing services are
     * allowed in standard mode after partial Clicker initialization failure.
     * The last characteristics have no descriptors/CCCD, so their value handles
     * are exactly the respective service ends. Handles never change on a mode
     * switch, and cached ends remain usable while the ATT range is hidden. */
    for (size_t i = 0; i < sizeof s_secure_services / sizeof s_secure_services[0]; ++i) {
        uint16_t start;
        rc = ble_gatts_find_svc(s_secure_services[i].uuid, &start);
        if (rc == BLE_HS_ENOENT && !active) continue;
        if (rc != 0) return rc;
        if (start != s_secure_services[i].start) {
            uint16_t end;
            rc = ble_gatts_find_chr(s_secure_services[i].uuid,
                                   s_secure_services[i].last_chr_uuid, NULL, &end);
            if (rc != 0) return rc;
            if (start == 0 || end < start) return BLE_HS_EINVAL;
            s_secure_services[i].start = start;
            s_secure_services[i].end = end;
            s_secure_services[i].visible = true;
        }
        if (start < first) first = start;
        if (s_secure_services[i].end > last) last = s_secure_services[i].end;
    }
    bool changed = false;
    rc = 0;
    for (size_t i = 0; i < sizeof s_secure_services / sizeof s_secure_services[0]; ++i) {
        if (s_secure_services[i].start == 0 || s_secure_services[i].visible == active) continue;
        rc = ble_gatts_svc_set_visibility(s_secure_services[i].start, active);
        if (rc != 0) break;
        s_secure_services[i].visible = active;
        changed = true;
        ESP_LOGI("clicker_hid", "GATT visibility=%d range=%u..%u", active,
                 s_secure_services[i].start, s_secure_services[i].end);
    }
    /* NimBLE stores only one pending Service Changed range. One bounded union
     * covers both HOGP services (and intervening DIS), never the earlier custom
     * service; two consecutive calls would overwrite the first range. Notify
     * even after a partial mutation failure so retry cannot lose invalidation. */
    if (changed) ble_svc_gatt_changed(first, last);
    return rc;
}

void platform_clicker_set_battery(int percent)
{
    if (percent >= 0 && percent <= 100) atomic_store(&s_battery_level, percent);
}

static int device_info_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    const void *value;
    size_t length;
    uint8_t battery;
    switch ((uintptr_t)arg) {
    case 1: value = s_pnp_id; length = sizeof s_pnp_id; break;
    case 2: value = "FoloToy"; length = sizeof("FoloToy") - 1; break;
    case 3: value = "AI PASSPORT"; length = sizeof("AI PASSPORT") - 1; break;
    case 4: {
        int level = atomic_load(&s_battery_level);
        if (level < 0) return BLE_ATT_ERR_UNLIKELY;
        battery = (uint8_t)level;
        value = &battery; length = sizeof battery;
        break;
    }
    default: return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    ESP_LOGI("clicker_hid", "HOGP device info read kind=%u bytes=%u",
             (unsigned)(uintptr_t)arg, (unsigned)length);
    return os_mbuf_append(ctxt->om, value, length) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_chr_def s_dis_characteristics[] = {
    {.uuid = &s_pnp_uuid.u, .access_cb = device_info_access, .arg = (void *)1,
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &s_manufacturer_uuid.u, .access_cb = device_info_access, .arg = (void *)2,
     .flags = BLE_GATT_CHR_F_READ},
    {.uuid = &s_model_uuid.u, .access_cb = device_info_access, .arg = (void *)3,
     .flags = BLE_GATT_CHR_F_READ},
    {0},
};
static const struct ble_gatt_chr_def s_bas_characteristics[] = {
    {.uuid = &s_battery_uuid.u, .access_cb = device_info_access, .arg = (void *)4,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC, .min_key_size = 16},
    {0},
};
static const struct ble_gatt_svc_def s_device_services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_dis_uuid.u,
     .characteristics = s_dis_characteristics},
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &s_bas_uuid.u,
     .characteristics = s_bas_characteristics},
    {0},
};

int ble_clicker_device_info_init(void)
{
    int rc = ble_gatts_count_cfg(s_device_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_device_services);
    return rc;
}
#else
void platform_clicker_set_battery(int percent) { (void)percent; }
#endif
