"""Compile the production adapter/event code with timed driver and transactional NVS fakes."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT = Path(__file__).resolve().parents[2]
FAKES = r'''
#define _POSIX_C_SOURCE 200809L
#include "xz_wifi_profiles.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef int nvs_handle_t;
typedef struct { struct {unsigned char ssid[32],password[64];int scan_method,sort_method,failure_retry_cnt;unsigned char channel;} sta;} wifi_config_t;
typedef struct {unsigned char ssid[33]; int8_t rssi;unsigned char primary,authmode;} wifi_ap_record_t;
typedef struct {bool show_hidden;int scan_type;struct {struct {unsigned min,max;} active;} scan_time;} wifi_scan_config_t;
typedef void (*xz_status_fn)(int,const char *);
typedef int esp_event_base_t;
typedef struct {unsigned reason;uint8_t ssid[32],ssid_len;} wifi_event_sta_disconnected_t;
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define WIFI_IF_STA 0
#define WIFI_SCAN_TYPE_ACTIVE 0
#define WIFI_ALL_CHANNEL_SCAN 1
#define WIFI_FAST_SCAN 0
#define WIFI_CONNECT_AP_BY_SIGNAL 1
#define WIFI_REASON_AUTH_FAIL 202
#define WIFI_REASON_AUTH_EXPIRE 2
#define WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT 15
#define WIFI_REASON_HANDSHAKE_TIMEOUT 204
#define WIFI_REASON_NO_AP_FOUND 201
#define WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210
#define WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211
#define WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD 212
#define WIFI_EVENT 1
#define IP_EVENT 2
#define IP_EVENT_STA_GOT_IP 1
#define WIFI_EVENT_STA_DISCONNECTED 2
#define ESP_LOGI(...) ((void)0)
#define XZ_WIFI 1
#define pdMS_TO_TICKS(x) (x)
static atomic_bool got_ip,connect_failed,wifi_connecting,disconnect_waiting,disconnect_received,wifi_attempt_issued,stop;
static atomic_uint disconnect_events;
static atomic_int failure_reason;
typedef enum { PORTAL_IDLE, PORTAL_CONNECTING, PORTAL_CONNECTED, PORTAL_FAILED } portal_state_t;
typedef enum { PORTAL_NONE, PORTAL_AUTH, PORTAL_NOT_FOUND, PORTAL_TIMEOUT, PORTAL_SAVE_FAILED, PORTAL_UNKNOWN } portal_error_t;
static int64_t now,connected_at,disconnect_at,second_disconnect_at;
static bool driver_active,commit_fail,scan_fail,namespace_missing;
static unsigned scenario,attempts,scan_index,cleared,sets,commits,scans,disconnect_calls;
static wifi_config_t active,legacy,driver_saved;
static wifi_ap_record_t recent_ap;
static uint8_t disk[XZ_WIFI_PROFILES_BLOB_SIZE],staged[XZ_WIFI_PROFILES_BLOB_SIZE]; static size_t disk_size;
static char tried[16];
static void wifi_event(void *,esp_event_base_t,int32_t,void *);
static int nvs_open(const char *name,int mode,nvs_handle_t *h){(void)name;(void)mode;*h=1;return namespace_missing?1:0;}
static void nvs_close(nvs_handle_t h){(void)h;}
static int nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
 (void)h;const void *src=NULL;size_t len=0;
 if(!strcmp(key,"wifi_profiles")){src=disk;len=disk_size;}
 if(!strcmp(key,"wifi")&&legacy.sta.ssid[0]){src=&legacy;len=sizeof legacy;}
 if(!len||*n<len)return 1;memcpy(out,src,len);*n=len;return 0;
}
static int nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t n){
 (void)h;assert(!strcmp(key,"wifi_profiles")&&n==sizeof disk);memcpy(staged,data,n);sets++;return 0;
}
static int nvs_commit(nvs_handle_t h){(void)h;commits++;if(commit_fail)return 1;memcpy(disk,staged,sizeof disk);disk_size=sizeof disk;return 0;}
static int64_t esp_timer_get_time(void){return now*1000;}
static void status(int phase,const char *text){(void)phase;(void)text;}
static int esp_wifi_disconnect(void){disconnect_calls++;if(!driver_active)return 1;driver_active=false;disconnect_at=now+150;if(scenario==7)second_disconnect_at=now+200;return 0;}
static int esp_wifi_get_config(int iface,wifi_config_t *cfg){(void)iface;*cfg=active.sta.ssid[0]?active:driver_saved;return 0;}
static int esp_wifi_sta_get_ap_info(wifi_ap_record_t *record){if(!driver_active)return 1;memset(record,0,sizeof *record);memcpy(record->ssid,active.sta.ssid,32);record->primary=6;return 0;}
static int esp_wifi_set_config(int iface,wifi_config_t *cfg){(void)iface;assert(!disconnect_waiting);active=*cfg;return 0;}
static int esp_wifi_connect(void){
 assert(!disconnect_waiting&&!driver_active);assert(active.sta.scan_method==WIFI_FAST_SCAN);
 tried[attempts++]=active.sta.ssid[0];connected_at=now;driver_active=true;
 if(scenario==1&&active.sta.ssid[0]=='B'){wifi_event_sta_disconnected_t e={.reason=WIFI_REASON_AUTH_FAIL};driver_active=false;wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e);}
 if(scenario==8){wifi_event_sta_disconnected_t e={.reason=WIFI_REASON_NO_AP_FOUND,.ssid_len=1};e.ssid[0]=active.sta.ssid[0];driver_active=false;wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e);}
 return 0;
}
static void vTaskDelay(unsigned ms){
 now+=ms;
 if(disconnect_at&&now>=disconnect_at){disconnect_at=0;wifi_event_sta_disconnected_t e={.reason=WIFI_REASON_AUTH_FAIL};wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e);}
 if(second_disconnect_at&&now>=second_disconnect_at){second_disconnect_at=0;wifi_event_sta_disconnected_t e={.reason=3};wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e);}
 if(driver_active&&((scenario==0||scenario==7)||(scenario==1&&active.sta.ssid[0]=='A')||(scenario==5&&now-connected_at>=5000)||(scenario==6&&active.sta.ssid[0]=='A')))
   wifi_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);
 if(scenario==4&&driver_active)stop=true;
}
static int esp_wifi_scan_start(wifi_scan_config_t *cfg,bool block){
 assert(!driver_active&&!disconnect_waiting);assert(block&&cfg->scan_time.active.max==100);scan_index=0;scans++;now+=1000;return scan_fail?1:0;
}
static int esp_wifi_scan_get_ap_record(wifi_ap_record_t *record){
 if(scenario==3||scan_fail||scan_index>=5)return 1;
 memset(record,0,sizeof *record);record->ssid[0]='A'+scan_index;
 record->rssi=-20-scan_index*10;record->primary=1;scan_index++;return 0;
}
static void esp_wifi_clear_ap_list(void){cleared++;}
static void reset(unsigned value){memset(&recent_ap,0,sizeof recent_ap);scenario=value;attempts=scan_index=cleared=scans=disconnect_calls=0;now=connected_at=disconnect_at=second_disconnect_at=0;driver_active=false;memset(&active,0,sizeof active);got_ip=connect_failed=wifi_connecting=disconnect_waiting=disconnect_received=wifi_attempt_issued=stop=false;disconnect_events=0;failure_reason=0;scan_fail=false;}
'''
CASES = r'''
int main(void){
 xz_wifi_profiles_t p={0},loaded;wifi_config_t cfg={0};
 memcpy(legacy.sta.ssid,"Legacy",6);memcpy(legacy.sta.password,"12345678",8);
 load_wifi_profiles(&loaded);assert(loaded.count==1&&!memcmp(loaded.items[0].ssid,"Legacy",6));
 memcpy(cfg.sta.ssid,"New",3);memcpy(cfg.sta.password,"abcdefgh",8);
 assert(save_wifi_profile(&loaded,&cfg)&&sets==1&&commits==1);
 assert(save_wifi_profile(&loaded,&cfg)&&sets==1);
 load_wifi_profiles(&p);assert(p.count==2&&!memcmp(p.items[0].ssid,"New",3));
 xz_wifi_profiles_t before=p;uint8_t old_disk[sizeof disk];memcpy(old_disk,disk,sizeof disk);
 commit_fail=true;cfg.sta.ssid[0]='X';assert(!save_wifi_profile(&p,&cfg));assert(!memcmp(&p,&before,sizeof p));assert(!memcmp(disk,old_disk,sizeof disk));commit_fail=false;
 disk[25]^=1;load_wifi_profiles(&p);assert(p.count==1&&!memcmp(p.items[0].ssid,"Legacy",6));
 namespace_missing=true;memcpy(driver_saved.sta.ssid,"SDK",3);memcpy(driver_saved.sta.password,"12345678",8);
 load_wifi_profiles(&p);assert(p.count==1&&!memcmp(p.items[0].ssid,"SDK",3));
 memset(driver_saved.sta.password,'x',64);load_wifi_profiles(&p);assert(!p.count);
 memset(&driver_saved,0,sizeof driver_saved);load_wifi_profiles(&p);assert(!p.count);
 namespace_missing=false;memcpy(driver_saved.sta.ssid,"SDK",3);load_wifi_profiles(&p);assert(p.count==1&&!memcmp(p.items[0].ssid,"Legacy",6));
 memset(&p,0,sizeof p);assert(xz_wifi_profiles_remember(&p,(uint8_t *)"A",1,"12345678"));assert(xz_wifi_profiles_remember(&p,(uint8_t *)"B",1,"12345678"));
 reset(0);assert(auto_wifi(&p,&stop,status,&cfg));assert(attempts==1&&tried[0]=='B'&&now==50&&scans==0);
 assert(active.sta.channel==0&&recent_ap.primary==6);
 wifi_ap_record_t previous_ap=recent_ap;reset(0);recent_ap=previous_ap;
 assert(auto_wifi(&p,&stop,status,&cfg)&&active.sta.channel==6&&scans==0);
 reset(0);recent_ap=previous_ap;recent_ap.ssid[0]='X';
 assert(auto_wifi(&p,&stop,status,&cfg)&&active.sta.channel==0);
 reset(1);assert(auto_wifi(&p,&stop,status,&cfg));assert(attempts==2&&tried[0]=='B'&&tried[1]=='A');assert(scans==1);
 for(char c='C';c<='E';c++)assert(xz_wifi_profiles_remember(&p,(uint8_t *)&c,1,"12345678"));
 reset(2);assert(!auto_wifi(&p,&stop,status,&cfg));assert(now==20000&&attempts==2&&!wifi_connecting&&!cfg.sta.ssid[0]);
 reset(3);assert(!auto_wifi(&p,&stop,status,&cfg));assert(now==20000&&attempts==2&&scans==1); // Empty scans still attempt saved profiles.
 reset(4);assert(!auto_wifi(&p,&stop,status,&cfg));assert(now==50&&attempts==1&&!wifi_connecting);
 memset(&p,0,sizeof p);assert(xz_wifi_profiles_remember(&p,(uint8_t *)"A",1,"12345678"));assert(xz_wifi_profiles_remember(&p,(uint8_t *)"B",1,"12345678"));
 reset(6);scan_fail=true;assert(auto_wifi(&p,&stop,status,&cfg));assert(attempts==2&&tried[0]=='B'&&tried[1]=='A'&&now<20000); // Failed scan does not erase fallback.
 reset(0);driver_active=true;assert(auto_wifi(&p,&stop,status,&cfg));assert(now==300&&attempts==1&&!connect_failed); // Delayed AUTH_FAIL from our own disconnect drained before connect.
 reset(7);driver_active=true;assert(auto_wifi(&p,&stop,status,&cfg));assert(now==350&&attempts==1&&!connect_failed&&!second_disconnect_at); // Both queued old callbacks drained before new attempt.
 reset(0);active.sta.ssid[0]='B';wifi_connecting=true;got_ip=true;
 wifi_event_sta_disconnected_t other={.reason=WIFI_REASON_AUTH_FAIL,.ssid={'A'},.ssid_len=1};
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&other);assert(got_ip&&!connect_failed&&!failure_reason); // Foreign SSID event cannot mutate this attempt.
 driver_active=true;wifi_event_sta_disconnected_t same={.reason=WIFI_REASON_AUTH_FAIL,.ssid={'B'},.ssid_len=1};
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&same);assert(got_ip&&!connect_failed); // Current association wins over queued same-SSID event.
 got_ip=false;active.sta.ssid[0]=0;wifi_event(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,NULL);assert(!got_ip); // Late IP cannot succeed on a mismatched/unconfigured association.
 reset(0);failure_reason=WIFI_REASON_AUTH_FAIL;connect_failed=true;assert(wifi_failure()==PORTAL_AUTH);failure_reason=WIFI_REASON_NO_AP_FOUND;assert(wifi_failure()==PORTAL_NOT_FOUND);failure_reason=0;connect_failed=false;assert(wifi_failure()==PORTAL_TIMEOUT);
 puts("wifi adapter: migration/COW, no-scan preferred, fallback, async disconnect, 20s cap, empty/failed scans and cancel PASS");
}
'''

def adapter_source():
 text=(ROOT/'components/platform/xiaozhi/xz_network.c').read_text()
 return text[text.index('static bool wifi_current_link'):text.index('void xz_identity')]+text[text.index('static void load_wifi_profiles'):text.index('bool xz_wifi_open')]

def run_case(case, fakes=FAKES):
 src=ROOT/'components/platform/xiaozhi'
 with tempfile.TemporaryDirectory() as folder:
  c,exe=Path(folder)/'auto.c',Path(folder)/'auto';c.write_text(fakes+adapter_source()+case)
  subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=undefined','-I',str(src),str(c),str(src/'xz_wifi_profiles.c'),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=10)

class AutoConnectTest(unittest.TestCase):
 def test_real_adapter(self):run_case(CASES)
 def test_same_saved_network_with_slow_dhcp_is_not_abandoned(self):
  run_case(r'''
int main(void){
 xz_wifi_profiles_t p={0};wifi_config_t cfg={0};
 assert(xz_wifi_profiles_remember(&p,(uint8_t *)"A",1,"12345678"));
 assert(xz_wifi_profiles_remember(&p,(uint8_t *)"B",1,"12345678"));
 (void)load_wifi_profiles;(void)save_wifi_profile;(void)wifi_failure;reset(5);
 assert(auto_wifi(&p,&stop,status,&cfg));
 assert(attempts==1&&tried[0]=='B'&&cfg.sta.ssid[0]=='B'&&now==5000&&scans==0);
 puts("same saved network survives five-second DHCP PASS");
}
''')

 def test_late_predecessor_event_cannot_abort_valid_dhcp(self):
  fakes=FAKES.replace('second_disconnect_at=now+200','second_disconnect_at=now+600').replace('(scenario==0||scenario==7)','(scenario==0||(scenario==7&&now-connected_at>=5000))')
  run_case(r'''
int main(void){
 xz_wifi_profiles_t p={0};wifi_config_t cfg={0};
 assert(xz_wifi_profiles_remember(&p,(uint8_t *)"A",1,"12345678"));
 (void)load_wifi_profiles;(void)save_wifi_profile;(void)wifi_failure;
 reset(7);driver_active=true;
 assert(auto_wifi(&p,&stop,status,&cfg));
 assert(attempts==1&&cfg.sta.ssid[0]=='A'&&now>=5000&&now<10000);
 puts("late 600ms predecessor disconnect cannot abort five-second DHCP PASS");
}
''',fakes)

 def test_idle_disconnect_success_without_event_does_not_block_saved_connect(self):
  fakes=FAKES.replace('if(!driver_active)return 1;driver_active=false;', 'if(!driver_active)return 0;driver_active=false;')
  run_case(r'''
int main(void){
 xz_wifi_profiles_t p={0};wifi_config_t cfg={0};
 assert(xz_wifi_profiles_remember(&p,(uint8_t *)"A",1,"12345678"));
 (void)load_wifi_profiles;(void)save_wifi_profile;(void)wifi_failure;reset(0);
 assert(auto_wifi(&p,&stop,status,&cfg));
 assert(attempts==1&&cfg.sta.ssid[0]=='A'&&now<1000&&!disconnect_waiting&&disconnect_calls==0);
 // An issued but not-yet-associated connection still must be cancelled. No
 // notification is guaranteed when that cancellation leaves no current AP.
 reset(0);wifi_attempt_issued=true;assert(wifi_quiesce(&stop,20000));
 assert(disconnect_calls==1&&!disconnect_events&&!disconnect_waiting&&now==1000);
 assert(auto_wifi(&p,&stop,status,&cfg)&&attempts==1);
 // A previous deadline may leave an empty notification barrier, never an AP lock.
 reset(0);disconnect_waiting=true;assert(wifi_quiesce(&stop,20000)&&!disconnect_waiting);
 puts("idle ESP_OK disconnect without event does not block saved connect PASS");
}
''',fakes)

if __name__=='__main__':unittest.main()
