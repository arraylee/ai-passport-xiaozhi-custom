"""Exercise actual HTTP/TLS error classification, including ESP-TLS sign rules."""
from pathlib import Path
import os, re, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]
FAKES=r'''
#include "cJSON.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_FAIL -1
#define HTTP_METHOD_POST 1
#define XZ_JSON_MAX 2048
#define XZ_CONSOLE_VERSION "2.1.0.42"
#define HTTP_EVENT_ON_DATA 1
#define MALLOC_CAP_8BIT 0
#define MBEDTLS_ERR_SSL_ALLOC_FAILED -0x7F00
#define MBEDTLS_ERR_X509_ALLOC_FAILED -0x2880
#define MBEDTLS_ERR_X509_CERT_VERIFY_FAILED -0x2700
#define ESP_LOGW(...) ((void)0)
typedef struct {void *user_data,*data;int event_id,data_len;} esp_http_client_event_t;
typedef struct {const char *url;int method;void *crt_bundle_attach;unsigned timeout_ms;
 int (*event_handler)(esp_http_client_event_t *);void *user_data;
 unsigned buffer_size,buffer_size_tx;bool disable_auto_redirect;} esp_http_client_config_t;
typedef esp_http_client_config_t *esp_http_client_handle_t;
static unsigned scenario,cleanups;static esp_http_client_config_t config;
static char certificate_bundle;
#define esp_crt_bundle_attach (&certificate_bundle)
static void xz_identity(char mac[18],char uuid[37]){strcpy(mac,"00:00:00:00:00:00");strcpy(uuid,"00000000-0000-0000-0000-000000000000");}
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg){
 assert(cfg->crt_bundle_attach&&cfg->disable_auto_redirect&&cfg->timeout_ms==6000);
 config=*cfg;return scenario==9?NULL:&config;
}
static void esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v){
 (void)h;if(!strcmp(k,"User-Agent"))assert(!strcmp(v,"ai-passport/2.1.0.42"));
}
static void esp_http_client_set_post_field(esp_http_client_handle_t h,const char *p,size_t n){(void)h;(void)p;(void)n;}
static int esp_http_client_perform(esp_http_client_handle_t h){
 if(scenario==1||scenario==5||scenario==6||scenario==7||scenario==8||scenario>=11)return ESP_FAIL;
 if(scenario==10)return ESP_ERR_NO_MEM;
 char *body=scenario==4?"broken":"{\"mqtt\":{}}";
 esp_http_client_event_t event={.user_data=h->user_data,.data=body,.event_id=HTTP_EVENT_ON_DATA,.data_len=(int)strlen(body)};
 return h->event_handler(&event);
}
static int esp_http_client_get_status_code(esp_http_client_handle_t h){(void)h;return scenario==2?503:scenario==3?403:200;}
static int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h,int *error,int *flags){
 (void)h;
 *flags=scenario==11?8:0;
 *error=scenario==5?0x7F00:scenario==6?-0x2880:scenario==7?0x2700:scenario==12?-0x2700:scenario==13?-0x0050:0;
 return scenario==8?ESP_ERR_NO_MEM:0;
}
static void esp_http_client_cleanup(esp_http_client_handle_t h){(void)h;cleanups++;}
'''
CASES=r'''
int main(void){
 cJSON *root=cloud_request("{}");assert(root&&cloud_failure==CLOUD_OK);cJSON_Delete(root);
 const cloud_error_t expected[]={CLOUD_OK,CLOUD_NETWORK,CLOUD_HTTP,CLOUD_HTTP,CLOUD_RESPONSE,CLOUD_MEMORY,CLOUD_MEMORY,CLOUD_TLS,CLOUD_MEMORY,CLOUD_MEMORY,CLOUD_MEMORY,CLOUD_TLS,CLOUD_TLS,CLOUD_TLS};
 for(scenario=1;scenario<=13;scenario++){
  assert(!cloud_request("{}")&&cloud_failure==expected[scenario]);
  assert(cloud_retryable==(scenario!=3&&scenario!=4&&scenario!=7&&scenario!=11&&scenario!=12));
 }
 assert(cleanups==13);
 puts("cloud request: verified TLS, HTTP/network/response classification, positive/negative TLS allocation and transport OOM PASS");
}
'''
class CloudRequestTest(unittest.TestCase):
 def test_http_and_tls_errors(self):
  src=ROOT/'components/platform/xiaozhi'
  text=(src/'xz_network.c').read_text()
  production=text[text.index('typedef struct {\n  char *body;'):text.index('static const char *cloud_failure_text')]
  idf=Path(os.environ.get('IDF_PATH',str(Path.home() / 'esp/esp-idf-v5.5.3')))/'components/json/cJSON'
  with tempfile.TemporaryDirectory() as folder:
   c,exe=Path(folder)/'request.c',Path(folder)/'request';c.write_text(FAKES+production+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(idf),str(c),str(idf/'cJSON.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
