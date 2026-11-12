"""Exercise shipped portal HTTP handlers and provisioning worker with real cJSON."""
from pathlib import Path
import os, subprocess, tempfile, unittest
try:
 from tests.host.test_xz_wifi_autoconnect import FAKES
 from tests.host.test_ble_advertising import function
except ModuleNotFoundError:
 from test_xz_wifi_autoconnect import FAKES
 from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include "cJSON.h"
#include "xz_portal.h"
#include <stdlib.h>
typedef int esp_err_t,esp_netif_t;
typedef void *httpd_handle_t,*esp_event_handler_instance_t;
typedef int httpd_err_code_t;
typedef struct {int held;} *SemaphoreHandle_t;
typedef struct {int content_len,received,code;const char *input,*query;bool gzip,no_cache,redirect,html;char response[2048];} httpd_req_t;
typedef struct {unsigned stack_size,max_open_sockets;bool lru_purge_enable;unsigned recv_wait_timeout;bool keep_alive_enable;int keep_alive_idle,keep_alive_interval,keep_alive_count;} httpd_config_t;
typedef struct {const char *uri;int method;esp_err_t (*handler)(httpd_req_t *);} httpd_uri_t;
typedef struct {int dynamic_rx_buf_num,static_rx_buf_num,ampdu_rx_enable,ampdu_tx_enable,rx_ba_win;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){.dynamic_rx_buf_num=3})
#define HTTPD_DEFAULT_CONFIG() ((httpd_config_t){0})
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_FAIL -1
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 0
#define WIFI_MODE_APSTA 1
#define WIFI_IF_AP 1
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_OWE 18
#define WIFI_PS_NONE 0
#define WIFI_PS_MIN_MODEM 1
typedef struct {struct {unsigned addr;} ip;} esp_netif_ip_info_t;
static esp_err_t esp_netif_get_ip_info(esp_netif_t*p,esp_netif_ip_info_t*i){(void)p;i->ip.addr=1;return 0;}
#define ESP_EVENT_ANY_ID -1
#define ESP_MAC_WIFI_STA 0
#define HTTP_GET 0
#define HTTP_POST 1
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_404_NOT_FOUND 404
#define HTTPD_RESP_USE_STRLEN -1
#define XZ_ERROR 3
#define XZ_SETUP 4
#define pdTRUE 1
static esp_netif_t *sta,*ap;
static httpd_handle_t http;
static esp_event_handler_instance_t wifi_handler,ip_handler;
static bool wifi_inited,net_started;
static atomic_bool credentials_ready;
static atomic_uint portal_outcome;
static wifi_config_t pending;
static SemaphoreHandle_t credential_guard;
static unsigned manual_case,submitted,failures_seen,success_polls,stops,ap_destroyed,dns_stops,registered,apple_probes,scan_stops;
static int64_t connected_seen,stop_at;
static unsigned welcome_calls;
static bool xz_wait_for_setup(atomic_bool *flag,xz_status_fn publish){(void)publish;welcome_calls++;return !atomic_load(flag);}
static bool sleep_started;
static unsigned phone_actions;
static bool note_activity(void){if(sleep_started)return false;phone_actions++;return true;}
static struct {bool (*portal_activity)(void);} config={note_activity};
static atomic_bool cancel;
static void portal_dns_poll(void);
static int esp_netif_init(void){return 0;}
static int esp_event_loop_create_default(void){return 0;}
static esp_netif_t *esp_netif_create_default_wifi_sta(void){return (void *)1;}
static esp_netif_t *esp_netif_create_default_wifi_ap(void){return (void *)2;}
static int esp_wifi_init(wifi_init_config_t *c){assert(c->dynamic_rx_buf_num==8&&c->static_rx_buf_num==4&&c->ampdu_rx_enable&&c->ampdu_tx_enable&&c->rx_ba_win==4);return 0;}
static int esp_wifi_set_storage(int s){(void)s;return 0;}
static int esp_event_handler_instance_register(int base,int id,void (*handler)(void *,int,int32_t,void *),void *arg,void **out){(void)base;(void)id;(void)arg;assert(handler==wifi_event);*out=(void *)1;return 0;}
static int esp_wifi_set_mode(int m){(void)m;return 0;}
static int esp_wifi_start(void){return 0;}
static int esp_wifi_set_ps(int m){assert(m==WIFI_PS_NONE);return 0;}
static void esp_read_mac(uint8_t *id,int type){(void)type;memset(id,0,6);}
static size_t strlcpy(char *out,const char *in,size_t n){size_t len=strlen(in);if(n){size_t count=len<n-1?len:n-1;memcpy(out,in,count);out[count]=0;}return len;}
static SemaphoreHandle_t xSemaphoreCreateMutex(void){static struct {int held;} mutex;mutex.held=0;return (void *)&mutex;}
static int xSemaphoreTake(SemaphoreHandle_t s,unsigned wait){(void)wait;if(s->held)return 0;s->held=1;return 1;}
static void xSemaphoreGive(SemaphoreHandle_t s){assert(s->held);s->held=0;}
static bool portal_dns_start(void){return true;}
static void portal_dns_stop(void){dns_stops++;}
static int httpd_start(httpd_handle_t *h,httpd_config_t *c){assert(c->max_open_sockets==4&&c->lru_purge_enable&&c->stack_size==4096);assert(c->keep_alive_enable&&c->keep_alive_idle==5&&c->keep_alive_interval==5&&c->keep_alive_count==3);*h=(void *)1;return 0;}
static int httpd_stop(httpd_handle_t h){assert(h);stops++;stop_at=now;return 0;}
static void esp_netif_destroy_default_wifi(esp_netif_t *p){assert(p==ap);ap_destroyed++;}
static int httpd_register_uri_handler(httpd_handle_t h,httpd_uri_t *uri){
 assert(h);if(!strcmp(uri->uri,"/wifi/status"))registered++;
 if(!strcmp(uri->uri,"/hotspot-detect.html")||!strcmp(uri->uri,"/library/test/success.html")){
  httpd_req_t req={0};assert(uri->method==HTTP_GET&&!uri->handler(&req));
  assert(req.code==200&&req.html&&req.gzip&&req.no_cache&&!req.redirect&&strlen(req.response));apple_probes++;
 }
 return 0;
}
static int httpd_register_err_handler(httpd_handle_t h,int code,int (*handler)(httpd_req_t *,int)){(void)h;(void)code;(void)handler;return 0;}
static int httpd_resp_set_hdr(httpd_req_t *r,const char *key,const char *value){
 if(!strcmp(key,"Cache-Control")){assert(!strcmp(value,"no-store"));r->no_cache=true;}
 else if(!strcmp(key,"Content-Encoding")){assert(!strcmp(value,"gzip"));r->gzip=true;}
 else {assert(!strcmp(key,"Location")&&!strcmp(value,"http://192.168.4.1/"));r->redirect=true;}
 return 0;
}
static int httpd_resp_set_type(httpd_req_t *r,const char *type){r->html=!strcmp(type,"text/html; charset=utf-8");return 0;}
static int httpd_resp_set_status(httpd_req_t *r,const char *s){r->code=atoi(s);return 0;}
static int httpd_resp_sendstr(httpd_req_t *r,const char *s){strlcpy(r->response,s,sizeof r->response);if(!r->code)r->code=200;return 0;}
static int httpd_resp_send(httpd_req_t *r,const char *s,int n){assert(n>0&&(size_t)n<sizeof r->response);memcpy(r->response,s,n);r->response[n]=0;if(!r->code)r->code=200;return 0;}
static int httpd_resp_send_err(httpd_req_t *r,int code,const char *s){r->code=code;return httpd_resp_sendstr(r,s);}
static int httpd_resp_sendstr_chunk(httpd_req_t *r,const char *s){size_t used=strlen(r->response);strlcpy(r->response+used,s,sizeof r->response-used);return 0;}
static int httpd_resp_send_chunk(httpd_req_t *r,const char *s,size_t n){(void)r;(void)s;(void)n;return 0;}
static int httpd_req_recv(httpd_req_t *r,char *out,int n){if(n>7)n=7;memcpy(out,r->input+r->received,n);r->received+=n;return n;}
static int httpd_req_get_url_query_str(httpd_req_t *r,char *out,size_t n){if(!r->query||strlen(r->query)>=n)return 1;strlcpy(out,r->query,n);return 0;}
static int esp_wifi_scan_stop(void){async_scan_at=0;scan_stops++;return 0;}
static const char portal[]="gzip-payload-boundary";
#define portal_end (portal+sizeof portal-1)
'''
CASES=r'''
static httpd_req_t post(const char *body){httpd_req_t req={.input=body,.content_len=(int)strlen(body)};assert(portal_post(&req)==0);return req;}
static void expect_state(const char *state,const char *error){
 httpd_req_t req={0};assert(!portal_status(&req));cJSON *j=cJSON_Parse(req.response);assert(j);assert(!strcmp(cJSON_GetObjectItem(j,"state")->valuestring,state));assert(!strcmp(cJSON_GetObjectItem(j,"error")->valuestring,error));assert(cJSON_GetArraySize(j)==2);assert(!strstr(req.response,"password")&&!strstr(req.response,"ssid"));cJSON_Delete(j);
}
static void phone_handoff(void){
 wifi_config_t saved=pending;unsigned outcome=portal_outcome,actions=phone_actions;
 bool connected=got_ip,connecting=wifi_connecting,failed=connect_failed,ready=credentials_ready;
 unsigned disconnects=disconnect_calls;
 wifi_event_ap_staconnected_t first={.aid=1},second={.aid=2};
 wifi_event_ap_stadisconnected_t left={.aid=1,.reason=8};
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,&first);
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STACONNECTED,&second);
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STADISCONNECTED,&left);
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_AP_STADISCONNECTED,NULL);
 assert(ap&&http&&!stops&&!dns_stops&&disconnects==disconnect_calls);
 assert(connected==got_ip&&connecting==wifi_connecting&&failed==connect_failed&&ready==credentials_ready);
 assert(outcome==portal_outcome&&!memcmp(&saved,&pending,sizeof pending));
 /* The remaining phone can open the page, without automatic probes cancelling sleep. */
 httpd_req_t page={0},probe={0};assert(!portal_get(&page)&&page.code==200);
 assert(!portal_redirect(&probe,404)&&probe.code==303&&actions==phone_actions);
}
static void portal_dns_poll(void){
 if(manual_case==8 && now<3600000){
  assert(ap&&http&&!stop&&!disk_size&&!attempts&&!submitted);
  if(now==300000||now==600000){unsigned before=phone_actions;expect_state("idle","none");phone_handoff();assert(before==phone_actions);}
  return;
 }
 if(manual_case==9){
  assert(ap&&http);
  if(now>=600000)stop=true;
  return;
 }
 if(!submitted){
  phone_handoff();
  httpd_req_t trailing=post("{\"ssid_hex\":\"42\",\"password\":\"12345678\"}junk");assert(trailing.code==400);
  httpd_req_t nul=post("{\"ssid_hex\":\"42\",\"password\":\"12345678\\u0000extra\"}");assert(nul.code==400);
  httpd_req_t invalid=post("{\"ssid_hex\":\"42\",\"password\":\"short\"}");assert(invalid.code==400);expect_state("idle","none");
  httpd_req_t first=post("{\"ssid_hex\":\"42\",\"password\":\"12345678\"}");assert(first.code==202);expect_state("connecting","none");
  wifi_config_t snapshot=pending;httpd_req_t repeat=post("{\"ssid_hex\":\"41\",\"password\":\"abcdefgh\"}");assert(repeat.code==409&&!memcmp(&snapshot,&pending,sizeof pending));
  phone_handoff(); // A phone leaving after submission must not cancel the submitted router connection.
  httpd_req_t scan={0};assert(!portal_networks(&scan)&&scan.code==503&&!scans);submitted=1;
 }
 unsigned state=atomic_load(&portal_outcome)>>8;
 if(state==PORTAL_CONNECTING&&atomic_load(&wifi_connecting))phone_handoff();
 if(state==PORTAL_FAILED){
  failures_seen++;
  if(manual_case==1){expect_state("failed","auth");assert(now>=20000&&now<=20100);}
  if(manual_case==7){expect_state("failed","not_found");assert(now>=20000&&now<=20100);}
  if(manual_case==2||manual_case==6)expect_state("failed","save_failed");
  if(manual_case==3){expect_state("failed","timeout");assert(now<=20100);}
  if(manual_case==5)expect_state("failed","auth");
  assert(!stops&&ap&&http);assert(!disk_size||manual_case==5); // Failed credentials never replace old storage.
  scenario=0;commit_fail=false;
  httpd_req_t retry=post("{\"ssid_hex\":\"41\",\"password\":\"\"}");assert(retry.code==202);submitted++;
 }
 if(state==PORTAL_CONNECTED){
  expect_state("connected","none");assert(disk_size==sizeof disk);success_polls++;if(!connected_seen)connected_seen=now;
  httpd_req_t repeat=post("{\"ssid_hex\":\"41\",\"password\":\"abcdefgh\"}");assert(repeat.code==409);
  if(manual_case==5&&submitted==1){driver_active=false;wifi_event_sta_disconnected_t e={.reason=WIFI_REASON_AUTH_FAIL};wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&e);}
 }
}
int main(void){
 /* Exercise real successful/failed scan paths and mutex release before worker cases. */
 credential_guard=xSemaphoreCreateMutex();
 portal_outcome_set(PORTAL_IDLE,PORTAL_NONE);
 httpd_req_t manual=post("{\"ssid\":\" Hide 家 \",\"password\":\"test-pass\"}");
 assert(manual.code==202&&!memcmp(pending.sta.ssid," Hide 家 ",10)&&!strcmp((char*)pending.sta.password,"test-pass"));
 credentials_ready=false;portal_outcome_set(PORTAL_IDLE,PORTAL_NONE);
 ap=(void *)2;reset(0);portal_outcome_set(PORTAL_IDLE,PORTAL_NONE);
 portal_scan=calloc(1,sizeof *portal_scan);assert(portal_scan);
 httpd_req_t scan={0};assert(!portal_networks(&scan)&&scan.code==202&&strstr(scan.response,"scanning")&&!scans&&!now);
 portal_scan_poll();assert(scans==1&&!now&&async_scan_at);
 /* HTTP navigation/probes and status remain responsive while the radio scans. */
 unsigned probe_actions=phone_actions;
 httpd_req_t probe={0};assert(!portal_get(&probe)&&probe.code==200&&probe.gzip&&probe.html&&probe.no_cache);
 httpd_req_t redirect={0};assert(!portal_redirect(&redirect,404)&&redirect.code==303&&redirect.redirect&&strlen(redirect.response));
 expect_state("idle","none");assert(phone_actions==probe_actions);
 scan=(httpd_req_t){0};assert(!portal_networks(&scan)&&scan.code==202&&scans==1&&!now&&phone_actions==probe_actions+1);
 vTaskDelay(1000);portal_scan_poll();assert(portal_scan->state==SCAN_READY&&!credential_guard->held);
 scan=(httpd_req_t){0};assert(!portal_networks(&scan));cJSON *list=cJSON_Parse(scan.response);assert(list&&cJSON_GetArraySize(cJSON_GetObjectItem(list,"networks"))==5);cJSON_Delete(list);
 unsigned cached_scans=scans;scan=(httpd_req_t){0};assert(!portal_networks(&scan)&&scans==cached_scans); // A second phone reuses the ready list.
 scan_fail=true;scan=(httpd_req_t){.query="refresh=1"};assert(!portal_networks(&scan)&&scan.code==202);portal_scan_poll();
 scan=(httpd_req_t){0};assert(!portal_networks(&scan)&&scan.code==503&&!credential_guard->held);scan_fail=false;
 /* A fresh request retries failure; a missing completion event has a bounded deadline. */
 scan=(httpd_req_t){.query="refresh=1"};assert(!portal_networks(&scan)&&scan.code==202);portal_scan_poll();
 async_scan_at=0;vTaskDelay(5000);portal_scan_poll();assert(portal_scan->state==SCAN_FAILED&&scan_stops);
 scan=(httpd_req_t){.query="refresh=1"};assert(!portal_networks(&scan)&&scan.code==202);portal_scan_poll();
 /* Manual hidden-SSID submit interrupts a running scan before association. */
 manual=post("{\"ssid\":\"Hidden\",\"password\":\"test-pass\"}");assert(manual.code==202);
 unsigned stopped=scan_stops;portal_scan_poll();assert(!async_scan_at&&scan_stops==stopped+1&&portal_scan->state==SCAN_IDLE);
 credentials_ready=false;portal_outcome_set(PORTAL_IDLE,PORTAL_NONE);
 wifi_event_sta_scan_done_t late={0};wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&late);portal_scan_poll();assert(portal_scan->state==SCAN_IDLE);
 scan=(httpd_req_t){0};assert(!portal_networks(&scan)&&scan.code==202);portal_scan_poll();assert(!portal_scan_done);
 vTaskDelay(1000);portal_scan_poll();assert(portal_scan->state==SCAN_READY);
 vTaskDelay(15000);scan=(httpd_req_t){0};assert(!portal_networks(&scan)&&scan.code==202);portal_scan_poll();
 portal_scan_release();assert(!portal_scan&&!async_scan_at);
 wifi_event(NULL,WIFI_EVENT,WIFI_EVENT_SCAN_DONE,&late);portal_scan_poll();assert(!portal_scan);
 assert(xSemaphoreTake(credential_guard,0));httpd_req_t locked=post("{\"ssid_hex\":\"41\",\"password\":\"12345678\"}");assert(locked.code==409&&!credentials_ready);xSemaphoreGive(credential_guard);
 /* Byte limits apply after JSON decoding; escaped literal slashes are preserved. */
 cJSON *obj=cJSON_CreateObject();char utf8[67]={0};for(unsigned i=0;i<21;i++)memcpy(utf8+3*i,"\xe4\xb8\x83",3);cJSON_AddStringToObject(obj,"ssid_hex","41");cJSON_AddStringToObject(obj,"password",utf8);char *encoded=cJSON_PrintUnformatted(obj);httpd_req_t bytes=post(encoded);assert(bytes.code==202&&strlen((char *)pending.sta.password)==63);free(encoded);cJSON_Delete(obj);
 credentials_ready=false;portal_outcome_set(PORTAL_IDLE,PORTAL_NONE);
 obj=cJSON_CreateObject();cJSON_AddStringToObject(obj,"ssid_hex","41");cJSON_AddStringToObject(obj,"password","AA\\u0000");encoded=cJSON_PrintUnformatted(obj);bytes=post(encoded);assert(bytes.code==202&&!strcmp((char *)pending.sta.password,"AA\\u0000"));free(encoded);cJSON_Delete(obj);
 credentials_ready=false;portal_outcome_set(PORTAL_IDLE,PORTAL_NONE);
 sleep_started=true;unsigned before=phone_actions;
 httpd_req_t paused_scan={0};assert(!portal_networks(&paused_scan)&&paused_scan.code==503);
 httpd_req_t paused_post=post("{\"ssid_hex\":\"41\",\"password\":\"\"}");
 assert(paused_post.code==503&&before==phone_actions&&!credentials_ready);
 sleep_started=false;
 cancel=true;httpd_req_t closed_scan={0};
 assert(!portal_networks(&closed_scan)&&closed_scan.code==503&&before==phone_actions);cancel=false;
 for(manual_case=0;manual_case<12;manual_case++){
  portal_scan_release();
  welcome_calls=0;
  reset(manual_case==1?1:manual_case==3?2:manual_case==4?4:manual_case==7?8:0);
  disk_size=0;memset(&legacy,0,sizeof legacy);memset(&driver_saved,0,sizeof driver_saved);commit_fail=manual_case==2||manual_case==6;if(manual_case==6||manual_case==11){memcpy(legacy.sta.ssid,"B",1);memcpy(legacy.sta.password,"12345678",8);}
  submitted=failures_seen=success_polls=stops=ap_destroyed=dns_stops=registered=apple_probes=0;connected_seen=stop_at=0;
  char hotspot[32],password[12];bool ok=xz_wifi_open(manual_case!=6 && manual_case!=10 && manual_case!=11,&stop,status,hotspot,password);
  assert(welcome_calls==(unsigned)(manual_case==10));
  if(manual_case==11){assert(ok && !registered && !submitted && attempts==1);continue;}
  if(manual_case==4){assert(!ok&&stop&&now<=200&&!stops);continue;}
  if(manual_case==9){assert(!ok&&stop&&now>=600000&&now<=600100&&!attempts);continue;}
  assert(ok&&registered==1&&apple_probes==2&&stops==1&&ap_destroyed==1&&dns_stops==1&&!http&&!ap&&!portal_scan);
  assert(stop_at-connected_seen>=3900&&success_polls>=39); // state was published one worker tick before first HTTP observation.
  if(manual_case==8)assert(now>=3604000&&submitted==1&&attempts==1);
  assert(failures_seen==(unsigned)(manual_case==1||manual_case==2||manual_case==3||manual_case==5||manual_case==6||manual_case==7));
 }
 puts("portal HTTP/worker: phone handoff preserves AP/credentials/router state, background probes do not refresh sleep, nonblocking scan, cache/retry/cancel/timeout, 4s AP grace PASS");
}
'''
class WifiHttpTest(unittest.TestCase):
 def test_production_http_and_worker(self):self.run_worker(FAKES)
 def test_idle_disconnect_ok_without_event_keeps_manual_and_scan_available(self):
  self.run_worker(FAKES.replace('if(!driver_active)return 1;driver_active=false;', 'if(!driver_active)return 0;driver_active=false;'))
 def run_worker(self,boundary):
  src=ROOT/'components/platform/xiaozhi';text=(src/'xz_network.c').read_text()
  fakes=boundary.replace('unsigned char ssid[32],password[64];int scan_method,sort_method,failure_retry_cnt;unsigned char channel;} sta;', 'unsigned char ssid[32],password[64];int scan_method,sort_method,failure_retry_cnt;unsigned char channel;} sta;struct {unsigned char ssid[32];unsigned ssid_len,max_connection,authmode,channel;} ap;')
  # AP config must not change the fake station profile.
  fakes=fakes.replace('(void)iface;assert(!disconnect_waiting);active=*cfg;', 'if(iface==1){assert(cfg->ap.max_connection==2&&cfg->ap.authmode==0);return 0;}assert(!disconnect_waiting);active=*cfg;')
  production=function((src/'xiaozhi.c').read_text(),'xz_portal_accept_input')+text[text.index('static void portal_outcome_set'):text.index('static wifi_config_t pending;')]+text[text.index('static bool wifi_current_link'):text.index('void xz_identity')]+text[text.index('static esp_err_t portal_get'):text.index('void xz_wifi_close')]
  idf=Path(os.environ.get('IDF_PATH',str(Path.home()/'esp/esp-idf-v5.5.3')))/'components/json/cJSON'
  with tempfile.TemporaryDirectory() as folder:
   c,exe=Path(folder)/'http.c',Path(folder)/'http';c.write_text(fakes+BOUNDARY+production+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=undefined','-I',str(src),'-I',str(idf),str(c),str(src/'xz_wifi_profiles.c'),str(src/'xz_portal.c'),str(idf/'cJSON.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=10)
if __name__=='__main__':unittest.main()
