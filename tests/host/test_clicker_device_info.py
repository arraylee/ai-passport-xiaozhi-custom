"""Walk the production HOGP companion services as a host would during discovery."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#define ESP_LOGI(...) ((void)0)
#define BLE_UUID16_INIT(n) { .u = {n} }
typedef struct { uint16_t value; } ble_uuid_t;
typedef struct { ble_uuid_t u; } ble_uuid16_t;
enum { BLE_GATT_ACCESS_OP_READ_CHR, BLE_GATT_ACCESS_OP_WRITE_CHR };
enum { BLE_GATT_SVC_TYPE_PRIMARY=1, BLE_GATT_CHR_F_READ=2, BLE_GATT_CHR_F_READ_ENC=4,
       BLE_ATT_ERR_READ_NOT_PERMITTED=5, BLE_ATT_ERR_WRITE_NOT_PERMITTED=6,
       BLE_ATT_ERR_INSUFFICIENT_RES=7, BLE_ATT_ERR_UNLIKELY=8 };
struct os_mbuf { uint8_t data[64]; size_t len; bool fail; };
struct ble_gatt_access_ctxt { unsigned op; struct os_mbuf *om; };
typedef int (*access_fn)(uint16_t,uint16_t,struct ble_gatt_access_ctxt *,void *);
struct ble_gatt_chr_def { const ble_uuid_t *uuid; access_fn access_cb; void *arg;
                         unsigned flags; unsigned min_key_size; };
struct ble_gatt_svc_def { unsigned type; const ble_uuid_t *uuid;
                         const struct ble_gatt_chr_def *characteristics; };
static int os_mbuf_append(struct os_mbuf *om,const void *data,size_t n) {
    if(om->fail) return 1;
    assert(n <= sizeof om->data); memcpy(om->data,data,n); om->len=n; return 0;
}
static const struct ble_gatt_svc_def *registered;
static int count_error, add_error, added;
static int ble_gatts_count_cfg(const struct ble_gatt_svc_def *s) { (void)s; return count_error; }
static int ble_gatts_add_svcs(const struct ble_gatt_svc_def *s) {
    registered=s; added++; return add_error;
}
// Visibility's real SDK/list behavior is exercised in test_ble_service_visibility.
#define BLE_HS_ENOENT 5
#define BLE_HS_EINVAL 3
static int ble_gatts_find_svc(const ble_uuid_t *uuid,uint16_t *handle) { return BLE_HS_ENOENT; }
static int ble_gatts_find_chr(const ble_uuid_t *svc,const ble_uuid_t *chr,uint16_t *def,uint16_t *val) { return BLE_HS_ENOENT; }
static int ble_gatts_svc_set_visibility(uint16_t handle,int visible) { return BLE_HS_ENOENT; }
static void ble_svc_gatt_changed(uint16_t first,uint16_t last) {}
'''

CASES = r'''
static const struct ble_gatt_chr_def *find(uint16_t service,uint16_t uuid) {
    for(const struct ble_gatt_svc_def *s=registered;s->type;s++) {
        if(s->uuid->value != service) continue;
        assert(s->type==BLE_GATT_SVC_TYPE_PRIMARY);
        for(const struct ble_gatt_chr_def *c=s->characteristics;c->uuid;c++)
            if(c->uuid->value==uuid) return c;
    }
    assert(!"Missing mandatory profile characteristic"); return NULL;
}
static struct os_mbuf read_char(const struct ble_gatt_chr_def *c) {
    struct os_mbuf out={0};
    struct ble_gatt_access_ctxt ctx={BLE_GATT_ACCESS_OP_READ_CHR,&out};
    assert(c->flags & BLE_GATT_CHR_F_READ);
    assert(c->access_cb(7,12,&ctx,c->arg)==0);
    return out;
}
int main(void) {
    count_error=21; assert(ble_clicker_device_info_init()==21 && !added);
    count_error=0; add_error=22; assert(ble_clicker_device_info_init()==22);
    add_error=0; assert(ble_clicker_device_info_init()==0);
    const struct ble_gatt_chr_def *pnp=find(0x180a,0x2a50), *bas=find(0x180f,0x2a19);
    struct os_mbuf value=read_char(pnp);
    const uint8_t expected[]={1,0xff,0xff,1,0,0x30,1}; // Internal preview ID, version 1.3.0 BCD.
    assert(value.len==7 && !memcmp(value.data,expected,7));
    value=read_char(find(0x180a,0x2a29));
    assert(value.len==7 && !memcmp(value.data,"FoloToy",7));
    value=read_char(find(0x180a,0x2a24));
    assert(value.len==11 && !memcmp(value.data,"AI PASSPORT",11));
    assert(bas->flags & BLE_GATT_CHR_F_READ_ENC); // HOGP battery is encrypted.
    assert(bas->min_key_size==16);
    // Invalid samples do not become fake 100%; valid levels retain full precision.
    for(int n=0;n<=100;n++) {
        platform_clicker_set_battery(n); value=read_char(bas);
        assert(value.len==1 && value.data[0]==n);
    }
    platform_clicker_set_battery(42);
    platform_clicker_set_battery(-1); platform_clicker_set_battery(101);
    value=read_char(bas); assert(value.data[0]==42);
    struct ble_gatt_access_ctxt ctx={BLE_GATT_ACCESS_OP_WRITE_CHR,&value};
    assert(pnp->access_cb(7,12,&ctx,pnp->arg)==BLE_ATT_ERR_WRITE_NOT_PERMITTED);
    assert(bas->access_cb(7,12,&ctx,bas->arg)==BLE_ATT_ERR_WRITE_NOT_PERMITTED);
    ctx.op=BLE_GATT_ACCESS_OP_READ_CHR; value.fail=true;
    assert(pnp->access_cb(7,12,&ctx,pnp->arg)==BLE_ATT_ERR_INSUFFICIENT_RES);
}
'''


class DeviceInfoTest(unittest.TestCase):
    def test_profile_is_registered_and_battery_is_sampled(self):
        source=(ROOT/'components/platform/platform_esp32/src/platform_clicker.c').read_text()
        self.assertIn('ble_clicker_device_info_init()', source)
        app=(ROOT/'components/app/src/app.c').read_text()
        sample=app.split('static bool battery_sample(void)',1)[1].split('static void battery_timer_cb',1)[0]
        self.assertIn('platform_clicker_set_battery',sample)

    def test_actual_service_discovery_values_and_errors(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            for name in ('platform/board_config.h','platform/platform_clicker.h',
                         'ble_internal.h','esp_log.h','host/ble_gatt.h','host/ble_hs.h','services/gatt/ble_svc_gatt.h'):
                p=tmp/name; p.parent.mkdir(parents=True,exist_ok=True)
                p.write_text('#pragma once\n')
            source=ROOT/'components/platform/platform_esp32/src/clicker_device_info.c'
            c=tmp/'profile.c'; exe=tmp/'profile'
            c.write_text(BOUNDARY+'\n#include "'+str(source)+'"\n'+CASES)
            subprocess.run(['cc','-std=c11','-DPERIPH_BLE=1','-UNDEBUG',
                            '-fsanitize=undefined','-I',str(tmp),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__': unittest.main()
