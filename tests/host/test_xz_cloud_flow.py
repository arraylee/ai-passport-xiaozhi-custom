"""Exercise real activation polling with a fake clock and transient requests."""
from pathlib import Path
import os,re,subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include "xz_network.h"
#include "xz_protocol.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#define ESP_LOGI(...) ((void)0)
static bool xz_cloud_cache_clear(void){return true;}
#define pdMS_TO_TICKS(x) (x)
#define SNTP_OPMODE_POLL 0
static bool sntp_started;
static unsigned scenario,calls,binds,errors;static uint64_t now;
static atomic_bool stop;
static char last[320];
static void status(xz_phase_t phase,const char *detail){if(phase==XZ_BIND)binds++;if(phase==XZ_ERROR)errors++;snprintf(last,sizeof last,"%s",detail);}
static int64_t esp_timer_get_time(void){return now*1000;}
static void vTaskDelay(unsigned n){now+=n;if(scenario==3||scenario==7)stop=true;}
static void esp_sntp_setoperatingmode(int n){(void)n;}
static void esp_sntp_setservername(int n,const char *s){(void)n;(void)s;}
static void esp_sntp_init(void){}
static time_t fake_time(time_t *out){time_t t=scenario==7?0:1800000000;if(out)*out=t;return t;}
#define time fake_time
void xz_identity(char mac[18],char uuid[37]){strcpy(mac,"00:00:00:00:00:00");strcpy(uuid,"00000000-0000-0000-0000-000000000000");}
static const struct {const char *version,*idf_ver;} app={"test","test"};
#define esp_app_get_description() (&app)
'''
REQUEST=r'''
static cloud_error_t cloud_failure;
static bool cloud_retryable;
static cJSON *cloud_request(const char *payload){
 (void)payload;calls++;cloud_retryable=true;cloud_failure=CLOUD_NETWORK;
 if(scenario==8&&calls>=2){cloud_retryable=false;cloud_failure=CLOUD_TLS;return NULL;}
 if(scenario==6&&calls>=6)return cJSON_Parse("{\"mqtt\":{}}");
 if(scenario==6&&calls>=2)return NULL;
 if(scenario==2){cloud_failure=CLOUD_MEMORY;return NULL;}
 if(scenario==3)return NULL;
 if(scenario==0&&(calls==1||calls==3))return NULL;
 if(scenario==0&&calls>=4)return cJSON_Parse("{\"mqtt\":{}}");
 if(scenario==5&&calls>=3)return cJSON_Parse("{\"mqtt\":{}}");
 if(scenario==5&&calls==2)return cJSON_Parse("{\"activation\":{\"code\":\"654321\"}}");
 if(scenario==4)return cJSON_Parse("{\"activation\":{\"code\":\"oops\"}}");
 return cJSON_Parse("{\"activation\":{\"code\":\"123456\"}}");
}
'''
CASES=r'''
int main(void){
 char code[24]={0};cJSON *root=xz_cloud_config(&stop,status,code);
 assert(root&&calls==4&&binds==1&&!errors&&!code[0]);cJSON_Delete(root);
 scenario=1;calls=binds=errors=0;now=0;code[0]=0;
 root=xz_cloud_config(&stop,status,code);assert(!root&&now>=1800000&&calls>=180);
 assert(binds==1&&errors==1&&strstr(last,"超时")); // No redraw on every poll.
 scenario=5;calls=binds=errors=0;now=0;code[0]=0;
 root=xz_cloud_config(&stop,status,code);
 assert(root&&calls==3&&binds==2&&!errors&&!code[0]);cJSON_Delete(root);
 scenario=6;calls=binds=errors=0;now=0;code[0]=0;
 root=xz_cloud_config(&stop,status,code);
 assert(root&&calls==6&&!errors&&!code[0]);cJSON_Delete(root); // Keep binding alive through a full transient retry batch.
 scenario=8;calls=binds=errors=0;now=0;code[0]=0;
 assert(!xz_cloud_config(&stop,status,code)&&calls==2&&errors==1&&now<30000&&strstr(last,"安全连接失败"));
 scenario=2;calls=binds=errors=0;now=0;code[0]=0;
 assert(!xz_cloud_config(&stop,status,code)&&calls==3&&errors==1&&strstr(last,"内存"));
 scenario=3;calls=binds=errors=0;now=0;code[0]=0;
 assert(!xz_cloud_config(&stop,status,code)&&calls==1&&!errors);stop=false;
 scenario=7;calls=binds=errors=0;now=0;code[0]=0;
 assert(!xz_cloud_config(&stop,status,code)&&stop&&!calls&&!errors&&now==100);stop=false;
 scenario=4;calls=binds=errors=0;now=0;code[0]=0;
 assert(!xz_cloud_config(&stop,status,code)&&!binds&&errors==1&&!code[0]);
 puts("cloud flow: 30 minute window, transient retries, stable code, memory error and cancellation PASS");
}
'''
class CloudFlowTest(unittest.TestCase):
 def test_polling_and_errors(self):
  src=ROOT/'components/platform/xiaozhi';text=(src/'xz_network.c').read_text()
  enums=re.search(r'typedef enum \{ CLOUD_OK[\s\S]*?\} cloud_error_t;',text)
  self.assertIsNotNone(enums,'typed cloud errors are required')
  tail=text[text.index('static const char *cloud_failure_text(void)'):]
  idf=Path(os.environ.get('IDF_PATH',str(Path.home() / 'esp/esp-idf-v5.5.3')))/'components/json/cJSON'
  with tempfile.TemporaryDirectory() as directory:
   c,exe=Path(directory)/'cloud.c',Path(directory)/'cloud'
   c.write_text(BOUNDARY+enums.group()+REQUEST+tail+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-fsanitize=undefined','-I',str(src),'-I',str(src/'include'),'-I',str(ROOT/'components/core/ports/include'),'-I',str(idf),str(c),str(src/'xz_protocol.c'),str(idf/'cJSON.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
