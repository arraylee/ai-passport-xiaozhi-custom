"""Actual capture hold/host-event policy with deterministic radio/controller boundaries.

No Bluetooth, USB or device calls. Negotiation acceptance is not actual parameters.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EALREADY 2
#define ESP_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define BLE_ERR_REM_USER_CONN_TERM 19
struct fake_mutex { bool held; };typedef struct fake_mutex StaticSemaphore_t;typedef StaticSemaphore_t *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *m){m->held=false;return m;}
static int xSemaphoreTake(SemaphoreHandle_t m,int ticks){assert(ticks==100);if(m->held)return 0;m->held=true;return pdTRUE;}
static void xSemaphoreGive(SemaphoreHandle_t m){assert(m->held);m->held=false;}
typedef int esp_err_t;typedef int hal_config_t;
static const char *TAG="capture-host";
static void quiet(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGI(...) quiet(__VA_ARGS__)
#define ESP_LOGW(...) quiet(__VA_ARGS__)
#define ESP_LOGE(...) quiet(__VA_ARGS__)
static const char *esp_err_to_name(int n){(void)n;return "synthetic";}
struct ble_gap_upd_params{uint16_t itvl_min,itvl_max,latency,supervision_timeout,min_ce_len,max_ce_len;};
struct ble_gap_conn_desc{uint16_t conn_itvl,conn_latency,supervision_timeout;};
struct ble_npl_event{bool queued;void(*cb)(struct ble_npl_event*);};
static struct ble_npl_event *queued_event;
static uint16_t s_conn=7;
static bool s_shutting_down,s_adv_mode_change_pending,closing,link_exists=true,in_host;
static int disable_error,enable_error,update_error;
static unsigned disables,enables,updates,shutdown_calls;
static bool s_stack_ready=true, stop_during_publish;
static int s_adv_retry_callout;
static void ble_npl_callout_stop(int *c){(void)c;shutdown_calls++;}
static void ble_clicker_begin_shutdown(void){}
static int ble_gap_disc_cancel(void){return 0;}
static int ble_gap_adv_stop(void){return 0;}
static int ble_gap_terminate(uint16_t c,int reason){(void)c;(void)reason;return 0;}
static void vTaskDelay(int ticks){assert(ticks==50);}
static int cfg_shutdown(hal_config_t *self);
static struct ble_gap_upd_params sent[8];
static struct ble_gap_conn_desc desc={12,22,400};
static void *nimble_port_get_dflt_eventq(void){return NULL;}
static void ble_npl_eventq_put(void *q,struct ble_npl_event *e){(void)q;if(stop_during_publish){stop_during_publish=false;assert(cfg_shutdown(NULL)!=0&&!shutdown_calls&&!s_shutting_down);}e->queued=true;queued_event=e;}
static void ble_npl_event_init(struct ble_npl_event *e,void(*cb)(struct ble_npl_event*),void *arg){(void)arg;e->cb=cb;}
static bool ble_clicker_connection_closing(uint16_t c){(void)c;return closing;}
static int esp_bt_sleep_disable(void){disables++;return disable_error;}
static int esp_bt_sleep_enable(void){enables++;return enable_error;}
static int ble_gap_conn_find(uint16_t c,struct ble_gap_conn_desc *d){assert(in_host);if(!link_exists||c!=s_conn)return 1;*d=desc;return 0;}
static int ble_gap_update_params(uint16_t c,const struct ble_gap_upd_params *p){assert(in_host&&c==s_conn&&updates<8);sent[updates++]=*p;return update_error;}
'''
CASES=r'''
static void drain(void){while(queued_event&&queued_event->queued){struct ble_npl_event *e=queued_event;e->queued=false;in_host=true;e->cb(e);in_host=false;}}
static void changed(void){
#ifdef CAPTURE_POLICY_PRESENT
 in_host=true;capture_params_connection_changed_host();in_host=false;
#endif
}
static void updated(int status){
#ifdef CAPTURE_POLICY_PRESENT
 in_host=true;capture_params_updated_host(s_conn,status);in_host=false;
#else
 (void)status;
#endif
 drain();
}
static void begin(void){assert(cfg_capture_transport_hold(NULL,true));drain();}
static void release(void){assert(cfg_capture_transport_hold(NULL,false));drain();}
int main(int argc,char **argv){assert(argc==2);int n=atoi(argv[1]);
#ifdef CAPTURE_POLICY_PRESENT
 ble_npl_event_init(&s_capture_params_event,capture_params_event_host,NULL);atomic_store(&s_capture_params_ready,true);changed();
#ifdef CAPTURE_PUBLISH_BARRIER_PRESENT
 s_capture_publish_mutex=xSemaphoreCreateMutexStatic(&s_capture_publish_storage);
#endif
#endif
 if(n==0){begin();assert(disables==1&&updates==1&&sent[0].latency==0&&sent[0].itvl_min==12&&sent[0].itvl_max==24);desc.conn_latency=0;updated(0);release();assert(enables==1&&updates==2&&sent[1].latency==22&&sent[1].itvl_min==12&&sent[1].itvl_max==12);desc.conn_latency=22;updated(0);release();assert(enables==1&&updates==2);}
 else if(n==1){begin();begin();assert(disables==1&&updates==1);release();assert(enables==1&&updates==1);desc.conn_latency=0;updated(0);assert(updates==2&&sent[1].latency==22);release();assert(enables==1);}
 else if(n==2){s_conn=BLE_HS_CONN_HANDLE_NONE;link_exists=false;changed();assert(!cfg_capture_transport_hold(NULL,true));drain();assert(!disables&&!enables&&!updates);release();}
 else if(n==3){update_error=9;begin();assert(disables==1&&updates==1);release();assert(enables==1&&updates==1);}
 else if(n==4){disable_error=8;assert(!cfg_capture_transport_hold(NULL,true));drain();assert(disables==1&&!enables&&!updates);release();assert(!enables);}
 else if(n==5){begin();desc.conn_latency=0;updated(0);enable_error=8;assert(!cfg_capture_transport_hold(NULL,false));drain();assert(enables==1);enable_error=0;release();assert(enables==2);}
 else if(n==6){begin();s_conn=BLE_HS_CONN_HANDLE_NONE;link_exists=false;changed();release();s_conn=7;link_exists=true;desc.conn_latency=30;changed();updated(0);assert(updates==1&&enables==1);}
 else if(n==7){assert(cfg_capture_transport_hold(NULL,true));s_conn=BLE_HS_CONN_HANDLE_NONE;changed();s_conn=7;desc.conn_latency=30;changed();drain();assert(!updates);release();assert(enables==1);}
 else if(n==8){desc.conn_latency=0;begin();assert(disables==1&&!updates);release();assert(enables==1&&!updates);}
 else if(n==9){begin();updated(9);release();assert(updates==1&&enables==1);}
 else if(n==10){begin();desc.conn_latency=0;updated(0);assert(cfg_capture_transport_hold(NULL,false));s_conn=BLE_HS_CONN_HANDLE_NONE;changed();s_conn=7;desc.conn_latency=30;changed();drain();assert(updates==1&&enables==1);}
 else if(n==11){begin();desc.conn_latency=0;updated(0);update_error=9;release();assert(updates==2&&enables==1);updated(9);assert(updates==2);}
 else if(n==12){begin();desc.conn_latency=0;updated(0);release();assert(updates==2);begin();assert(updates==2);desc.conn_latency=22;updated(0);assert(updates==3&&sent[2].latency==0);desc.conn_latency=0;updated(0);release();assert(updates==4&&sent[3].latency==22&&disables==2&&enables==2);}
 else if(n==13){begin();desc.conn_latency=0;updated(0);update_error=BLE_HS_EALREADY;release();assert(updates==2&&enables==1);update_error=0;updated(0);assert(updates==3&&sent[2].latency==22);updated(9);assert(updates==3);}
 else if(n==14){desc.conn_latency=0;begin();assert(!updates);desc.conn_latency=22;updated(0);assert(updates==1&&sent[0].latency==0);desc.conn_latency=0;updated(0);desc.conn_latency=22;updated(0);assert(updates==1);release();assert(enables==1&&updates==2&&sent[1].latency==0);}
 else if(n==15){
#ifdef CAPTURE_PUBLISH_BARRIER_PRESENT
 s_capture_publish_mutex->held=true;
#endif
 assert(cfg_shutdown(NULL)!=0&&!shutdown_calls&&!s_shutting_down);
#ifdef CAPTURE_PUBLISH_BARRIER_PRESENT
 s_capture_publish_mutex->held=false;
#endif
 assert(cfg_shutdown(NULL)==0&&shutdown_calls==1);assert(!cfg_capture_transport_hold(NULL,true));}
 else if(n==16){stop_during_publish=true;begin();assert(updates==1);assert(cfg_shutdown(NULL)==0&&shutdown_calls==1);assert(cfg_capture_transport_hold(NULL,false));drain();assert(updates==1&&enables>=1);}
 else assert(false);return 0;
}
'''
class BleCaptureTransport(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cls.tmp=tempfile.TemporaryDirectory(prefix='passport-capture-transport-');cls.addClassCleanup(cls.tmp.cleanup)
  src=(ROOT/'components/platform/platform_esp32/src/ble_config.c').read_text()
  block=''
  if '// Capture parameter policy: host-event-owned' in src:
   block='#define CAPTURE_POLICY_PRESENT 1\n'+src.split('// Capture parameter policy: host-event-owned',1)[1].split('\n',1)[1].split('// End capture parameter policy',1)[0]
  if 'static bool capture_params_close_for_stop(void)' in src:block='#define CAPTURE_PUBLISH_BARRIER_PRESENT 1\n'+block
  code=BOUNDARY+function(src,'config_connection_ready')+'\n'+block+function(src,'cfg_capture_transport_hold')+function(src,'cfg_shutdown')+CASES
  c=Path(cls.tmp.name)/'capture.c';c.write_text(code);cls.exe=Path(cls.tmp.name)/'capture'
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-fsanitize=undefined','-fno-sanitize-recover=undefined',str(c),'-o',str(cls.exe)],check=True,timeout=30)
 def case(self,n):subprocess.run([str(self.exe),str(n)],check=True,timeout=5)
 def test_low_latency_request_and_restore_only_after_capture(self):self.case(0)
 def test_duplicate_hold_release_and_release_before_update_complete(self):self.case(1)
 def test_no_connection_never_disables_sleep(self):self.case(2)
 def test_update_request_failure_degrades_without_leaking_modem_hold(self):self.case(3)
 def test_sleep_disable_failure_never_requests_update(self):self.case(4)
 def test_sleep_enable_failure_is_reported_and_retryable(self):self.case(5)
 def test_disconnect_and_reused_handle_drop_old_restore(self):self.case(6)
 def test_queued_hold_cannot_update_replacement_connection(self):self.case(7)
 def test_already_low_latency_never_negotiates_unnecessarily(self):self.case(8)
 def test_async_peer_rejection_does_not_retry_or_fail_modem_release(self):self.case(9)
 def test_queued_release_cannot_restore_replacement_connection(self):self.case(10)
 def test_restore_error_is_bounded_and_releases_modem(self):self.case(11)
 def test_next_capture_waits_for_inflight_restore_then_requests_once(self):self.case(12)
 def test_restore_busy_defers_once_to_next_gap_update_without_polling(self):self.case(13)
 def test_initial_low_latency_then_central_slowdown_allows_one_capture_request(self):self.case(14)
 def test_shutdown_publication_barrier_failure_never_stops_host(self):self.case(15)
 def test_shutdown_cannot_deinit_event_inside_inflight_publish(self):self.case(16)
if __name__=='__main__':unittest.main()
