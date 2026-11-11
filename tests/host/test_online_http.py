"""Run the production HTTP loop with streaming, cancellation and failing transports."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_xz_channel_memory import function

ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef bool (*xz_body_fn)(const char*,size_t,void*);
typedef struct {char location[256];bool icy;} header_t;
typedef struct {
 const char *url,*user_agent;int timeout_ms,buffer_size,buffer_size_tx,crt_bundle_attach,tls_dyn_buf_strategy;
 bool disable_auto_redirect,keep_alive_enable;int (*event_handler)(void*);void *user_data;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
#define HTTP_TLS_DYN_BUF_RX_STATIC 1
#define ESP_OK 0
#define ESP_ERR_HTTP_EAGAIN 9
#define esp_crt_bundle_attach 123
#define ESP_LOGI(tag,fmt,...) ((void)(tag),(void)printf(fmt,__VA_ARGS__))
static unsigned scenario,opens,closed,reads,received,loans,returns,timeout_changes;
static bool owned,stop;
static int64_t now;
static bool xz_codec_memory_loan_begin(void){if(scenario==7)return false;assert(!owned);owned=true;loans++;return true;}
static bool xz_codec_memory_loan_end(void){assert(owned&&closed==opens);owned=false;returns++;return true;}
static bool xz_online_cancelled(void){return stop;}
static bool public_dns(const char *url){return strstr(url,"://example.org")!=NULL||strstr(url,"://api.open-meteo.com")!=NULL;}
static bool xz_url_public(const char *url){return !strncmp(url,"https://",8);}
static int header(void *e){(void)e;return 0;}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){
 assert(c->tls_dyn_buf_strategy==HTTP_TLS_DYN_BUF_RX_STATIC);
 assert(c->buffer_size==1024&&c->buffer_size_tx==512&&c->crt_bundle_attach==123);
 assert(c->disable_auto_redirect&&c->keep_alive_enable);opens++;
 if(scenario==4){strcpy(((header_t*)c->user_data)->location,"/redirect.mp3");}
 return (void*)1;
}
static void esp_http_client_set_header(void*c,const char*k,const char*v){assert(c&&((!strcmp(k,"Icy-MetaData")&&!strcmp(v,"0"))||(!strcmp(k,"Accept-Encoding")&&!strcmp(v,"identity"))));}
static void esp_http_client_set_timeout_ms(void*c,int ms){assert(c&&ms==300&&scenario!=8);timeout_changes++;}
static int esp_http_client_open(void*c,int n){assert(c&&!n);return scenario==1?-1:0;}
static int esp_http_client_fetch_headers(void*c){assert(c);return scenario==2?-1:0;}
static int esp_http_client_get_status_code(void*c){assert(c);return scenario==4?302:scenario==5?503:200;}
static int64_t esp_timer_get_time(void){return now+=100000;}
static int esp_http_client_read(void*c,char*b,int size){
 assert(c&&size==1024);reads++;
 if(scenario==3){stop=true;return -1;}
 if(scenario==9||scenario==10){
  now+=300000;
  if(scenario==10||reads%3)return -ESP_ERR_HTTP_EAGAIN;
  if(reads>18)return 0;
  memset(b,'a',size);return size;
 }
 if(reads>3)return 0;
 memset(b,'a',size);return size;
}
static bool esp_http_client_is_complete_data_received(void*c){assert(c);return true;}
static void esp_http_client_cleanup(void*c){assert(c&&closed<opens);closed++;}
static bool receive(const char*b,size_t n,void*ctx){assert(b&&n==1024&&!ctx);received++;return scenario!=6;}
'''
CASES=r'''
int main(void){
 for(scenario=0;scenario<11;scenario++){
  opens=closed=reads=received=loans=returns=timeout_changes=0;now=0;owned=stop=false;
  bool ok=xz_http_visit(scenario==8?"https://api.open-meteo.com/v1/forecast":"https://example.org/stream.mp3",receive,NULL);
  assert(closed==opens&&!owned&&loans==returns);
  assert(ok==(scenario==0||scenario==6||scenario==7||scenario==8||scenario==9));
  assert(timeout_changes==(unsigned)(scenario==0||scenario==3||scenario==6||scenario==7||scenario==9||scenario==10));
  if(scenario==0||scenario==7||scenario==8)assert(received==3&&reads==4);
  if(scenario==6)assert(received==1&&reads==1);
  if(scenario==4)assert(opens==4);
  if(scenario==9)assert(received==6&&reads==21&&now>8000000);
  if(scenario==10)assert(!received&&reads>=18&&reads<=21);
 }
 opens=closed=loans=returns=0;owned=stop=false;scenario=0;
 assert(!xz_http_visit("https://not-allowed.invalid/x",receive,NULL));
 assert(!opens&&loans==returns&&!owned);
 puts("HTTP fixed TLS RX, stream delivery, redirects, cancellation and every cleanup path PASS");
}
'''

class OnlineHttpTest(unittest.TestCase):
    def test_bounded_stream_transport_and_loan_cleanup(self):
        code=(ROOT/'components/platform/xiaozhi/xz_web.c').read_text()
        with tempfile.TemporaryDirectory() as folder:
            src=Path(folder)/'http.c';exe=Path(folder)/'http'
            src.write_text(BOUNDARY+function(code,'visit')+function(code,'xz_http_visit')+CASES)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=10)

if __name__=='__main__':unittest.main()
