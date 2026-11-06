"""Reproduce the device's full subscription table without retaining bond keys."""
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
#define ESP_LOGI(...) ((void)0)
enum { BLE_HS_ENOENT=5, BLE_HS_EBUSY=15, BLE_HS_ESTORE_FAIL=30,
       BLE_STORE_OBJ_TYPE_OUR_SEC=1, BLE_STORE_OBJ_TYPE_PEER_SEC=2,
       BLE_STORE_OBJ_TYPE_CCCD=3, BLE_STORE_OBJ_TYPE_CSFC=8 };
typedef struct { uint8_t type, val[6]; } ble_addr_t;
static const ble_addr_t any={255,{0}};
#define BLE_ADDR_ANY (&any)
struct ble_store_key_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle; uint8_t idx; };
struct ble_store_key_csfc { ble_addr_t peer_addr; uint8_t idx; };
struct ble_store_key_sec { ble_addr_t peer_addr; };
struct ble_store_value_cccd { ble_addr_t peer_addr; uint16_t chr_val_handle, flags; };
union ble_store_key { struct ble_store_key_sec sec; struct ble_store_key_cccd cccd; struct ble_store_key_csfc csfc; };
union ble_store_value { struct { ble_addr_t peer_addr; } sec,csfc; struct ble_store_value_cccd cccd; };
static union ble_store_value records[5];
static int count=5, reads_fail, deletes_fail, deleted, sec_deletes;
static bool scan, advertising;
static int ble_gap_disc_active(void) { return scan; }
static int ble_gap_adv_active(void) { return advertising; }
static int ble_store_read(int kind,const union ble_store_key *key,union ble_store_value *out) {
    if(reads_fail)return 30;
    if(kind==1 || kind==2)return key->sec.peer_addr.val[0]==1 ? 0:BLE_HS_ENOENT;
    if(kind==8)return BLE_HS_ENOENT;
    assert(kind==3 && key->cccd.peer_addr.type==255);
    if(key->cccd.idx>=count)return BLE_HS_ENOENT;
    *out=records[key->cccd.idx]; return 0;
}
static int ble_store_delete(int kind,const union ble_store_key *key) {
    if(kind==1 || kind==2){sec_deletes++;assert(false);}
    assert(kind==3); if(deletes_fail)return 30;
    for(int i=0;i<count;i++)if(records[i].cccd.peer_addr.val[0]==key->cccd.peer_addr.val[0] &&
                             records[i].cccd.chr_val_handle==key->cccd.chr_val_handle){
        memmove(records+i,records+i+1,(count-i-1)*sizeof(*records));count--;deleted++;return 0;
    }
    return BLE_HS_ENOENT;
}
'''
CASES = r'''
int main(void) {
    for(int i=0;i<5;i++) {
        records[i].cccd.peer_addr.val[0]=(i==0?1:(i<3?2:3));
        records[i].cccd.chr_val_handle=(i==2||i==4)?31:8;
    }
    scan=true; assert(ble_clicker_prune_orphan_subscriptions()==15 && !deleted); scan=false;
    advertising=true; assert(ble_clicker_prune_orphan_subscriptions()==15 && !deleted); advertising=false;
    reads_fail=1; assert(ble_clicker_prune_orphan_subscriptions()==30 && !deleted); reads_fail=0;
    deletes_fail=1; assert(ble_clicker_prune_orphan_subscriptions()==30 && !deleted); deletes_fail=0;
    assert(ble_clicker_prune_orphan_subscriptions()==0);
    assert(count==1 && deleted==4 && !sec_deletes && records[0].cccd.peer_addr.val[0]==1);
    assert(ble_clicker_prune_orphan_subscriptions()==0 && deleted==4);
}
'''

class BondStoreTest(unittest.TestCase):
    def test_existing_device_orphans_and_failure_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory)
            for name in ('platform/board_config.h','ble_internal.h','esp_log.h',
                         'host/ble_gap.h','host/ble_store.h'):
                p=tmp/name; p.parent.mkdir(parents=True,exist_ok=True);p.write_text('#pragma once\n')
            source=ROOT/'components/platform/platform_esp32/src/clicker_bond_store.c'
            c=tmp/'store.c'; exe=tmp/'store'
            c.write_text(BOUNDARY+'\n#include "'+str(source)+'"\n'+CASES)
            subprocess.run(['cc','-std=c11','-DPERIPH_BLE=1','-UNDEBUG','-fsanitize=undefined',
                            '-I',str(tmp),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__=='__main__': unittest.main()
