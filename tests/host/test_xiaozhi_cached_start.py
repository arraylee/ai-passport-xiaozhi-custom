"""Run the production cached-connect/fallback owner with exact teardown boundaries."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_ble_advertising import function

ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include "xz_network.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define TAG "test"
static atomic_bool cancel;
static xz_view_t working;
typedef struct {void *mqtt;int udp,aes;atomic_bool connected,control_ready,closing;atomic_int error;} channel_t;
static unsigned scenario,opened,https,saved,cleared,clients,sockets,contexts,released,errors,binds;
static void status(xz_phase_t phase,const char *detail){working.phase=phase;if(phase==XZ_BIND)binds++;if(phase==XZ_ERROR)errors++;(void)detail;}
static void codec_arena_metric(unsigned step){assert(step==3);}
static void mbedtls_aes_init(int *p){*p=7;contexts++;}
static void mbedtls_aes_free(int *p){assert(*p==7&&contexts);contexts--;}
static void esp_mqtt_client_stop(void *p){assert(p==(void*)1&&clients==1);}
static void esp_mqtt_client_destroy(void *p){assert(p==(void*)1&&clients==1);clients--;}
static int close(int fd){assert(fd==17&&sockets==1);sockets--;return 0;}
static void channel_clear(channel_t *c){assert(!clients&&!sockets);(void)c;released++;}
static bool command(channel_t*c,const char*t,const char*s){(void)c;(void)t;(void)s;assert(false);return false;}
static const char *channel_error_text(channel_t*c){assert(c);return "connection unavailable";}
static bool xz_cloud_cache_save(const cJSON *cloud){assert(cloud);saved++;return scenario!=9;}
static bool xz_cloud_cache_clear(void){cleared++;return true;}
cJSON *xz_cloud_config(atomic_bool *stop,xz_status_fn fn,char code[24]){
 assert(!clients&&!sockets&&!contexts&&!atomic_load(stop));https++;
 if(scenario==2){strcpy(code,"123456");fn(XZ_BIND,"Bind this device");return NULL;}
 if(scenario==3){fn(XZ_ERROR,"Network unavailable");return NULL;}
 return cJSON_Parse("{\"mqtt\":{\"fresh\":true}}");
}
'''
OPEN=r'''
static bool open_channel(channel_t*c,cJSON **cloud){
 assert(*cloud&&!clients&&!sockets);opened++;cJSON_Delete(*cloud);*cloud=NULL;
 c->mqtt=(void*)1;clients++;c->udp=17;sockets++;c->connected=true;
 if(scenario==5){cancel=true;return false;}
 if(scenario==6||(scenario!=0&&scenario!=9&&scenario!=10&&scenario!=11&&opened==1)){
  c->error=scenario==7?CHANNEL_MEMORY:scenario==8?CHANNEL_TLS:CHANNEL_AUTH;return false;
 }
 return true;
}
'''
CASES=r'''
int main(void){
 for(scenario=0;scenario<=11;scenario++){
  opened=https=saved=cleared=clients=sockets=contexts=released=errors=binds=0;cancel=false;
  working=(xz_view_t){0};cJSON *cloud=cJSON_Parse("{\"mqtt\":{}}");assert(cloud);
  bool cached=scenario!=0&&scenario!=6&&scenario!=9;
  if(scenario==10)cancel=true;
  channel_t *channel=open_session_channel(&cloud,cached,true);
  if(scenario==0||scenario==1||scenario==8||scenario==9||scenario==11){
    assert(channel&&!errors&&!binds);assert(opened==((scenario==1||scenario==8)?2U:1U));
    assert(https==((scenario==1||scenario==8)?1U:0U));assert(saved==(scenario==11?0U:1U));
    assert(cleared==(scenario==1?1U:0U));channel_release(channel,false);
  }else{
    assert(!channel&&!clients&&!sockets&&!contexts);
    assert(https==((scenario==2||scenario==3)?1U:0U));
    assert(binds==(scenario==2?1U:0U)); // Only an activation response prompts binding.
    assert(errors==((scenario==3||scenario==4||scenario==6||scenario==7)?1U:0U));
  }
  cJSON_Delete(cloud);assert(!clients&&!sockets&&!contexts);
 }
 puts("Cached start: no HTTPS on hit, one refresh on stale auth/TLS, complete teardown before HTTPS, activation-only binding, cancellation, OOM and failed cache writes PASS");
}
'''


class CachedStartTest(unittest.TestCase):
    def test_direct_start_and_bounded_refresh(self):
        text=(ROOT / "components/platform/xiaozhi/xiaozhi.c").read_text()
        errors=re.search(r"enum \{ CHANNEL_OK[\s\S]*?\};",text).group()
        # Scenario4 fails allocation before opening a channel; no retry is useful.
        alloc=r'''
static void *try_calloc(size_t n,size_t size){return scenario==4?NULL:calloc(n,size);}
#define calloc try_calloc
'''
        code=BOUNDARY+errors+OPEN+function(text,"reconnect_status")+function(text,"channel_release")+alloc+function(text,"open_session_channel")+CASES
        sdk=Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
        component=ROOT / "components/platform/xiaozhi"
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory) / "connect.c";source.write_text(code)
            executable=Path(directory) / "connect"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-deprecated-declarations",
                "-Wno-unused-variable","-fsanitize=address,undefined","-I"+str(component),"-I"+str(component / "include"),
                "-I"+str(ROOT / "components/core/ports/include"),"-I"+str(sdk),str(source),str(sdk / "cJSON.c"),
                "-o",str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=10)


if __name__=="__main__":
    unittest.main()
