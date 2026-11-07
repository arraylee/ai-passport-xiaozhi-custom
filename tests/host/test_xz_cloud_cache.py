"""Run the real credential cache with cJSON, allocation failures and durable NVS doubles."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NVS = r'''
#pragma once
#include <stddef.h>
typedef int nvs_handle_t,esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND -1
#define NVS_READONLY 0
#define NVS_READWRITE 1
int nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
int nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
int nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
int nvs_erase_key(nvs_handle_t,const char *);
int nvs_commit(nvs_handle_t);
'''
BOUNDARY = r'''
#include "xz_cloud_cache.h"
#include "xz_network.h"
#include "nvs.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define XZ_CONSOLE_VERSION "2.1.0.42"
static char disk[5000],pending[5000],uuid[37]="12345678-1234-1234-1234-123456789abc";
static size_t length,pending_length;
static int writes,commits,handles,fail_open,fail_write,fail_commit,read_race;
static unsigned allocations,fail_at,live;
static void *test_malloc(size_t n){if(++allocations==fail_at)return NULL;void*p=malloc(n);if(p)live++;return p;}
static void test_free(void*p){if(p){assert(live);live--;free(p);}}
void xz_identity(char mac[18],char id[37]){strcpy(mac,"00:00:00:00:00:01");strcpy(id,uuid);}
int nvs_open(const char*n,int mode,int*h){assert(!strcmp(n,"xiaozhi")&&(mode==0||mode==1));if(fail_open)return -2;*h=1;handles++;return 0;}
void nvs_close(int h){assert(h==1&&handles==1);handles--;}
int nvs_get_blob(int h,const char*k,void*d,size_t*n){
 assert(h==1&&!strcmp(k,"mqtt_cache_v1"));if(!length)return -1;
 if(!d){*n=length;return 0;}
 if(read_race){*n=9000;return -3;}
 if(*n<length){*n=length;return -3;}
 memcpy(d,disk,length);*n=length;return 0;
}
int nvs_set_blob(int h,const char*k,const void*d,size_t n){
 assert(h==1&&!strcmp(k,"mqtt_cache_v1")&&n<=4096);writes++;
 if(fail_write)return -2;memcpy(pending,d,n);pending_length=n;return 0;
}
int nvs_erase_key(int h,const char*k){assert(h==1&&!strcmp(k,"mqtt_cache_v1"));if(!length)return -1;pending_length=0;return 0;}
int nvs_commit(int h){assert(h==1);commits++;if(fail_commit)return -2;memcpy(disk,pending,pending_length);length=pending_length;return 0;}
#define malloc test_malloc
#define free test_free
'''
CASES = r'''
#undef malloc
#undef free
static cJSON *fixture(void){return cJSON_Parse("{\"mqtt\":{\"endpoint\":\"mqtt.example.invalid:8883\",\"client_id\":\"client\",\"username\":\"user\",\"password\":\"fake-password\",\"publish_topic\":\"up\",\"subscribe_topic\":\"down\",\"unneeded\":\"discard-this\"},\"udp\":{\"key\":\"never-cache-session-key\"},\"firmware\":{\"url\":\"discard-this-too\"}}");}
int main(void){
 cJSON_Hooks hooks={.malloc_fn=test_malloc,.free_fn=test_free};cJSON_InitHooks(&hooks);
 assert(!xz_cloud_cache_load());assert(xz_cloud_cache_clear());
 cJSON *source=fixture();assert(source);unsigned baseline=live;
 assert(xz_cloud_cache_save(source));assert(writes==1&&commits==1&&live==baseline&&!handles);
 assert(!strstr(disk,"never-cache-session-key")&&!strstr(disk,"discard-this"));
 cJSON *loaded=xz_cloud_cache_load();assert(loaded && xz_cloud_cache_version_current(loaded));
 cJSON_ReplaceItemInObject(loaded,"reported_firmware",cJSON_CreateString("2.1.0.41"));
 assert(!xz_cloud_cache_version_current(loaded));
 cJSON_DeleteItemFromObject(loaded,"reported_firmware");
 assert(!xz_cloud_cache_version_current(loaded));cJSON_Delete(loaded);assert(live==baseline);
 assert(xz_cloud_cache_save(source)&&writes==1&&commits==1); // Deduplicated durable writes.
 uuid[0]='x';assert(!xz_cloud_cache_load());uuid[0]='1';
 char saved[5000];size_t saved_len=length;memcpy(saved,disk,length);
 const char *bad[]={"{", "{}", "{\"version\":2}", "{\"version\":1} trailing", "null"};
 for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++){strcpy(disk,bad[i]);length=strlen(disk)+1;assert(!xz_cloud_cache_load());assert(live==baseline);}
 memcpy(disk,saved,saved_len);length=saved_len;
 disk[length-1]='x';assert(!xz_cloud_cache_load());disk[length-1]=0;
 length=5000;assert(!xz_cloud_cache_load());length=saved_len;
 read_race=1;assert(!xz_cloud_cache_load());assert(xz_cloud_cache_save(source));read_race=0;
 assert(live==baseline); // NVS changing its reported length must never overrun cleanup.
 cJSON *mqtt=cJSON_GetObjectItem(source,"mqtt");
 cJSON_ReplaceItemInObject(mqtt,"endpoint",cJSON_CreateString("tcp://insecure"));assert(!xz_cloud_cache_save(source));
 cJSON_ReplaceItemInObject(mqtt,"endpoint",cJSON_CreateString("mqtt.example.invalid:8883"));
 cJSON_AddObjectToObject(source,"activation");assert(!xz_cloud_cache_save(source));cJSON_DeleteItemFromObject(source,"activation");
 cJSON_ReplaceItemInObject(mqtt,"password",cJSON_CreateString("changed-secret"));
 fail_write=1;assert(!xz_cloud_cache_save(source));fail_write=0;
 fail_commit=1;assert(!xz_cloud_cache_save(source));fail_commit=0;
 loaded=xz_cloud_cache_load();assert(loaded);assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(loaded,"mqtt"),"password")->valuestring,"fake-password"));cJSON_Delete(loaded);
 fail_open=1;assert(!xz_cloud_cache_load()&&!xz_cloud_cache_save(source)&&!xz_cloud_cache_clear());fail_open=0;
 assert(xz_cloud_cache_save(source));baseline=live;
 for(unsigned i=1;i<70;i++){
   allocations=0;fail_at=i;(void)xz_cloud_cache_save(source);fail_at=0;assert(live==baseline&&!handles);
   allocations=0;fail_at=i;loaded=xz_cloud_cache_load();fail_at=0;cJSON_Delete(loaded);assert(live==baseline&&!handles);
 }
 assert(xz_cloud_cache_clear()&&!xz_cloud_cache_load());cJSON_Delete(source);assert(!live&&!handles);
 puts("Durable MQTT cache: identity, bounded whitelist, malformed/oversized data, no session keys, unchanged-write dedup, failed commit, NVS length race and all allocation failures PASS");
}
'''


class CloudCacheTest(unittest.TestCase):
    def test_real_cache_storage_and_faults(self):
        component=ROOT / "components/platform/xiaozhi"
        sdk=Path(os.environ["IDF_PATH"]) / "components/json/cJSON"
        with tempfile.TemporaryDirectory() as directory:
            directory=Path(directory)
            (directory / "nvs.h").write_text(NVS)
            source=directory / "cache.c"
            source.write_text(BOUNDARY + (component / "xz_cloud_cache.c").read_text() + CASES)
            executable=directory / "cache"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-misleading-indentation",
                "-Wno-deprecated-declarations","-fsanitize=address,undefined","-I"+str(directory),"-I"+str(component),
                "-I"+str(component / "include"),"-I"+str(ROOT / "components/core/ports/include"),
                "-I"+str(sdk),str(source),str(sdk / "cJSON.c"),"-o",str(executable)],check=True)
            subprocess.run([str(executable)],check=True,timeout=10)


if __name__ == "__main__":
    unittest.main()
