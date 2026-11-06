"""Real NimBLE hidden ATT ranges plus production pre-advertising mode gate.

No radio, pairing, persistence or hardware is accessed. SDK list/move/find/read
and visibility functions execute unchanged; permission/application callbacks and
HCI are observable boundaries. Physical OS cache handling remains device QA.
"""
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import test_ble_advertising as advertising

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / 'components/platform/platform_esp32/src'

def sdk_function(source, name):
    matches = list(re.finditer(r'(?m)^' + name + r'\(', source))
    match = matches[-1] if matches else None
    if not match:
        return advertising.function(source, name)
    start = source.rfind('\n', 0, match.start() - 1) + 1
    end = source.index('\n}\n', match.end()) + 2
    text = source[start:end]
    return re.sub(r'\A(?:#[^\n]*\n|\s*\n)+', '', text)

def host_path():
    idf = os.environ.get('IDF_PATH')
    if not idf:
        idf = json.loads((ROOT/'build/project_description.json').read_text())['idf_path']
    return Path(idf)/'components/bt/host/nimble/nimble/nimble/host'

def run_c(code, includes=()):
    with tempfile.TemporaryDirectory(prefix='passport-visibility-') as d:
        d = Path(d)
        (d/'syscfg').mkdir()
        (d/'syscfg/syscfg.h').write_text('#define MYNEWT_VAL(name) 0\n')
        c = d/'test.c'; c.write_text(code)
        cmd = ['cc','-std=c11','-UNDEBUG','-fsanitize=undefined','-fno-sanitize-recover=all','-I',str(d)]
        for inc in includes: cmd += ['-I',str(inc)]
        result = subprocess.run(cmd + [str(c),'-o',str(d/'test')],capture_output=True,text=True,timeout=20)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)
        result = subprocess.run([str(d/'test')],capture_output=True,text=True,timeout=5)
        if result.returncode: raise AssertionError(result.stdout+result.stderr)

SDK_BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/queue.h>
#ifndef STAILQ_REMOVE_AFTER
#define STAILQ_REMOVE_AFTER(head,elm,field) do { \
 if ((STAILQ_NEXT(elm,field)=STAILQ_NEXT(STAILQ_NEXT(elm,field),field))==NULL) \
 (head)->stqh_last=&STAILQ_NEXT(elm,field); } while(0)
#endif
#define MYNEWT_VAL(name) 0
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_ENOENT 5
#define BLE_HS_EAPP 9
#define BLE_ATT_ERR_INVALID_HANDLE 1
#define BLE_ATT_ACCESS_OP_READ 1
#define BLE_HS_DBG_ASSERT assert
struct os_mbuf { int unused; };
typedef struct {uint16_t value;} ble_uuid_t;
struct ble_att_svr_entry;
typedef int ble_att_svr_access_fn(uint16_t,uint16_t,int,uint16_t,struct os_mbuf **,void *);
struct ble_att_svr_entry {
 STAILQ_ENTRY(ble_att_svr_entry) ha_next;
 const ble_uuid_t *ha_uuid; uint8_t ha_flags,ha_min_key_size;
 uint16_t ha_handle_id; ble_att_svr_access_fn *ha_cb; void *ha_cb_arg;
};
STAILQ_HEAD(ble_att_svr_entry_list,ble_att_svr_entry);
static struct ble_att_svr_entry_list ble_att_svr_list,ble_att_svr_hidden_list;
struct ble_gatts_svc_entry {uint16_t handle,end_group_handle;};
static struct ble_gatts_svc_entry ble_gatts_svc_entries[]={{28,45},{53,55}};
static int ble_gatts_num_svc_entries=2;
static unsigned permissions,callbacks;
static int permission_error;
static int ble_uuid_cmp(const ble_uuid_t *a,const ble_uuid_t *b) {return a->value!=b->value;}
static int ble_att_svr_check_perms(uint16_t c,int read,struct ble_att_svr_entry *e,uint8_t *err) {
 assert(c==7 && read==1); ++permissions; *err=permission_error; return permission_error;
}
static int accessed(uint16_t c,uint16_t a,int op,uint16_t off,struct os_mbuf **om,void *arg) {
 (void)c;(void)a;(void)op;(void)off;(void)om;(void)arg; ++callbacks; return 0;
}
'''

class VisibilityTest(unittest.TestCase):
    def test_real_sdk_hidden_cached_handles_never_reach_security_or_callback(self):
        h = host_path()
        att = (h/'src/ble_att_svr.c').read_text()
        gatts = (h/'src/ble_gatts.c').read_text()
        funcs = '\n'.join(sdk_function(att,n) for n in (
            'ble_att_svr_find_by_handle','ble_att_svr_find_by_uuid','ble_att_svr_read',
            'ble_att_svr_read_handle','ble_att_svr_move_entries','ble_att_svr_hide_range','ble_att_svr_restore_range'))
        funcs += sdk_function(gatts,'ble_gatts_svc_set_visibility')
        run_c(SDK_BOUNDARY+funcs+r'''
int main(void) {
 STAILQ_INIT(&ble_att_svr_list); STAILQ_INIT(&ble_att_svr_hidden_list);
 struct ble_att_svr_entry entries[7]={0};
 uint16_t handles[]={21,23,28,29,45,53,55};
 for(unsigned i=0;i<7;i++){entries[i].ha_handle_id=handles[i];entries[i].ha_cb=accessed;
  entries[i].ha_flags=5;entries[i].ha_min_key_size=16;
  STAILQ_INSERT_TAIL(&ble_att_svr_list,&entries[i],ha_next);}
 struct os_mbuf om={0};uint8_t err=0;
 permission_error=15; assert(ble_att_svr_read_handle(7,29,0,&om,&err)==15 && permissions==1 && !callbacks);
 assert(ble_gatts_svc_set_visibility(28,0)==0);
 assert(ble_gatts_svc_set_visibility(53,0)==0);
 for(unsigned j=0;j<2;j++){
  assert(ble_att_svr_read_handle(7,j?55:29,0,&om,&err)==BLE_HS_ENOENT && err==BLE_ATT_ERR_INVALID_HANDLE);
  assert(permissions==1 && !callbacks);
 }
 unsigned count=0;struct ble_att_svr_entry *e=NULL;
 while((e=ble_att_svr_find_by_uuid(e,NULL,65535))){assert(e->ha_handle_id==21||e->ha_handle_id==23);++count;}
 assert(count==2); // Custom public services remain discoverable, exact hidden ranges absent.
 assert(ble_gatts_svc_set_visibility(28,0)==0); // Idempotent list movement.
 assert(ble_gatts_svc_set_visibility(28,1)==0);
 assert(ble_gatts_svc_set_visibility(53,1)==0);
 permission_error=0;assert(ble_att_svr_read_handle(7,29,0,&om,&err)==0 && callbacks==1 && permissions==2);
 assert(entries[3].ha_flags==5 && entries[3].ha_min_key_size==16);
 assert(ble_gatts_svc_set_visibility(999,0)==BLE_HS_ENOENT);
}
''')

    def test_production_hogp_visibility_lifecycle_with_real_sdk_pending_changed_range(self):
        h = host_path()
        gatts = (h/'src/ble_gatts.c').read_text()
        gatt_service = (h/'services/gatt/src/ble_svc_gatt.c').read_text()
        production = (SRC/'clicker_device_info.c').read_text()
        constants = production[production.index('static const ble_uuid16_t s_dis_uuid'):production.index('void platform_clicker_set_battery')]
        boundary = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#define MYNEWT_VAL(name) 0
#define BLE_HS_ENOENT 5
#define BLE_HS_EINVAL 3
#define BLE_UUID16_INIT(n) {.u={n}}
#define ESP_LOGI(...) ((void)0)
typedef struct {uint16_t value;} ble_uuid_t;
typedef struct {ble_uuid_t u;} ble_uuid16_t;
struct ble_gatt_svc_def {const ble_uuid_t *uuid;};
struct ble_gatts_svc_entry {const struct ble_gatt_svc_def *svc;uint16_t handle,end_group_handle;};
static const ble_uuid_t hid={0x1812},bas={0x180f};
static struct ble_gatt_svc_def defs[]={{&hid},{&bas}};
static struct ble_gatts_svc_entry ble_gatts_svc_entries[]={{&defs[0],28,45},{&defs[1],53,55}};
static int ble_gatts_num_svc_entries=2;
static bool visible[2]={true,true};
static unsigned hide_calls,restore_calls,changed_calls,find_chr_calls;
static uint16_t fail_handle;
static int find_chr_error;
static uint16_t ble_svc_gatt_start_handle,ble_svc_gatt_end_handle;
static uint16_t ble_svc_gatt_changed_val_handle=8;
static int ble_uuid_cmp(const ble_uuid_t *a,const ble_uuid_t *b){return a->value!=b->value;}
static void ble_att_svr_hide_range(uint16_t first,uint16_t last){
 assert((first==28&&last==45)||(first==53&&last==55));
 visible[first==53]=false;++hide_calls;
}
static void ble_att_svr_restore_range(uint16_t first,uint16_t last){
 assert((first==28&&last==45)||(first==53&&last==55));
 visible[first==53]=true;++restore_calls;
}
static void ble_gatts_chr_updated(uint16_t handle){assert(handle==8);++changed_calls;}
static int ble_gatts_find_chr(const ble_uuid_t *svc,const ble_uuid_t *chr,uint16_t *def,uint16_t *value){
 assert(def==NULL);++find_chr_calls;
 if(find_chr_error)return find_chr_error;
 assert((svc->value==0x1812&&chr->value==0x2a4c)||(svc->value==0x180f&&chr->value==0x2a19));
 assert(visible[svc->value==0x180f]); // Lookup cannot work after ATT hide.
 *value=svc->value==0x1812?45:55;return 0;
}
'''
        # Execute the complete real init, including its earliest failure return;
        # timers/SDK registration are boundaries, not a duplicate init function.
        init_boundary = r'''
#define ESP_OK 0
#define BLE_HS_ENOMEM 6
#define BLE_SM_IO_CAP_NO_IO 0
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define PLATFORM_CLICKER_OFF 0
#define ESP_LOGE(...) ((void)0)
static bool s_shutting_down,s_host_synced,s_report_callout_ready,s_initialized;
static int s_gate,s_reports,s_work_event,s_report_callout;
static void *s_bridge_timer;
static int init_name_error;
static struct {int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_our_key_dist,sm_their_key_dist;
 int (*store_status_cb)(void *,void *);} ble_hs_cfg;
typedef struct {void (*callback)(void *);const char *name;} esp_timer_create_args_t;
static int clicker_init_name(void){return init_name_error;}
static void clicker_gate_init(int *p){} static void clicker_reports_init(int *p){}
static void work_event_cb(void *p){} static void report_callout_cb(void *p){}
static void bridge_timer_cb(void *p){}
static void ble_npl_event_init(int *p,void (*cb)(void *),void *arg){}
static void *nimble_port_get_dflt_eventq(void){return NULL;}
static void ble_npl_callout_init(int *p,void *q,void(*cb)(void *),void *arg){}
static int esp_timer_create(const esp_timer_create_args_t *args,void **out){*out=(void *)1;return 0;}
static int store_status(void *a,void *b){return 0;}
static int s_hid_services[1];
static int ble_gatts_count_cfg(const void *p){return 0;}
static int ble_gatts_add_svcs(const void *p){return 0;}
static int ble_clicker_device_info_init(void){return 0;}
static void snapshot_set_status(int status){}
static const char *ble_clicker_name(void){return "test";}
'''
        funcs = sdk_function(gatts,'ble_gatts_find_svc_entry')+sdk_function(gatts,'ble_gatts_find_svc')
        funcs += sdk_function(gatts,'ble_gatts_svc_set_visibility').replace('ble_gatts_svc_set_visibility(', 'sdk_visibility(')
        funcs += r'''
static int ble_gatts_svc_set_visibility(uint16_t handle,int active){
 if(handle==fail_handle)return 42;return sdk_visibility(handle,active);
}
'''
        funcs += sdk_function(gatt_service,'ble_svc_gatt_changed')
        actual_init = advertising.function((SRC/'platform_clicker.c').read_text(),'ble_clicker_init')
        run_c(boundary+funcs+constants+init_boundary+actual_init+r'''
static void check_pending(void){assert(ble_svc_gatt_start_handle==28&&ble_svc_gatt_end_handle==55);}
int main(void){
 ble_clicker_services_reset();
 // First standard advertisement must hide both; custom range below28 stays out of SC.
 assert(ble_clicker_prepare_services_host(false)==0 && !visible[0]&&!visible[1]);
 assert(hide_calls==2&&changed_calls==1&&find_chr_calls==2);check_pending();
 assert(ble_clicker_prepare_services_host(false)==0&&changed_calls==1&&find_chr_calls==2);
 // Host reset preserves existing database/cache: restore uses cached ends, not hidden ATT lookup.
 assert(ble_clicker_prepare_services_host(true)==0&&visible[0]&&visible[1]);
 assert(restore_calls==2&&changed_calls==2&&find_chr_calls==2);check_pending();
 assert(ble_clicker_prepare_services_host(true)==0&&restore_calls==2&&changed_calls==2);
 // Failed second service never consumes its state; union remains complete on partial and retry.
 fail_handle=53;assert(ble_clicker_prepare_services_host(false)==42);
 assert(!visible[0]&&visible[1]&&hide_calls==3&&changed_calls==3);check_pending();
 fail_handle=0;assert(ble_clicker_prepare_services_host(false)==0);
 assert(!visible[0]&&!visible[1]&&hide_calls==4&&changed_calls==4);check_pending();
 // Voice full deinit/reinit creates default-visible fresh database, resets cached handles.
 visible[0]=visible[1]=true;init_name_error=47;
 assert(ble_clicker_init()==47); // Even earliest init failure resets old hidden-cache state.
 assert(ble_clicker_prepare_services_host(false)==0&&!visible[0]&&!visible[1]);
 assert(find_chr_calls==4&&hide_calls==6&&changed_calls==5);check_pending();
 // Resolve failure must occur before ANY mutation; no fake successful advertising result.
 visible[0]=visible[1]=true;ble_clicker_services_reset();find_chr_error=43;
 assert(ble_clicker_prepare_services_host(false)==43&&visible[0]&&visible[1]&&hide_calls==6);
 find_chr_error=0;
 // Partial init with only HIDS registered must hide it despite Clicker being unavailable.
 ble_gatts_num_svc_entries=1;ble_clicker_services_reset();
 assert(ble_clicker_prepare_services_host(false)==0&&!visible[0]&&visible[1]);
 assert(ble_svc_gatt_start_handle==28&&ble_svc_gatt_end_handle==45);
 assert(ble_clicker_prepare_services_host(true)==BLE_HS_ENOENT&&!visible[0]);
 // Neither service registered: standard configuration may still advertise.
 ble_gatts_num_svc_entries=0;ble_clicker_services_reset();
 unsigned before=changed_calls;assert(ble_clicker_prepare_services_host(false)==0&&changed_calls==before);
 assert(ble_clicker_prepare_services_host(true)==BLE_HS_ENOENT);
}
''')

    def test_production_advertising_requires_visibility_gate_before_radio_start(self):
        h = host_path()
        sdk_adv = (h/'src/ble_hs_adv.c').read_text()
        sdk_uuid = (h/'src/ble_uuid.c').read_text()
        source = (SRC/'ble_config.c').read_text()
        code = advertising.BOUNDARY.replace(
            'static int ble_clicker_prepare_services_host(bool active) { (void)active; return 0; }', '')+r'''
static int prepare_calls,prepare_error;
static bool prepared_mode;
static int ble_clicker_prepare_services_host(bool active) {
 assert(s_conn==BLE_HS_CONN_HANDLE_NONE && !radio_active);++prepare_calls;prepared_mode=active;return prepare_error;
}
'''
        code += advertising.function(sdk_uuid,'ble_uuid_flat')
        code += '\n'.join(advertising.function(sdk_adv,n) for n in (
            'ble_hs_adv_set_hdr','ble_hs_adv_set_flat_mbuf','ble_hs_adv_set_array_uuid16',
            'ble_hs_adv_set_array_uuid32','ble_hs_adv_set_array_uuid128','ble_hs_adv_set_array16',
            'adv_set_fields','ble_hs_adv_set_fields'))
        code += advertising.function(source,'schedule_adv_retry')+advertising.function(source,'start_adv')
        code += r'''
static int gap_event(struct ble_gap_event *e,void *p){return 0;}
const char *ble_clicker_name(void){return "QA";}
int main(void) {
 clicker_mode=false; start_adv(); assert(prepare_calls==1 && !prepared_mode && radio_active);
 radio_active=false;clicker_mode=true;start_adv();assert(prepare_calls==2 && prepared_mode && radio_active);
 radio_active=false;prepare_error=42;start_adv();assert(prepare_calls==3 && !radio_active && starts==2);
 assert(reported_error==42 && s_adv_retry_callout.due);
 prepare_error=0;s_conn=7;start_adv();assert(prepare_calls==3 && !radio_active);
 s_conn=BLE_HS_CONN_HANDLE_NONE;s_shutting_down=true;start_adv();assert(prepare_calls==3);
 s_shutting_down=false;s_host_synced=false;start_adv();assert(prepare_calls==3);
}
'''
        run_c(code,(h/'include',ROOT/'components/platform/platform_esp32/include',ROOT/'components/core/services/include'))

if __name__ == '__main__': unittest.main()
