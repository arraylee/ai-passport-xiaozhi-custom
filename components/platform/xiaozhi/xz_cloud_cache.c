#include "xz_cloud_cache.h"
#include "xz_network.h"
#include "nvs.h"
#include <stdlib.h>
#include <string.h>

#define CACHE_MAX 4096u
static const char cache_key[] = "mqtt_cache_v1";
static const struct { const char *name; size_t capacity; } fields[] = {
    {"endpoint",192}, {"client_id",256}, {"username",512}, {"password",768},
    {"publish_topic",256}, {"subscribe_topic",256},
};
static bool mqtt_valid(const cJSON *cloud) {
    if (cJSON_GetObjectItemCaseSensitive(cloud,"activation")) return false;
    const cJSON *mqtt = cJSON_GetObjectItemCaseSensitive(cloud,"mqtt");
    if (!cJSON_IsObject(mqtt)) return false;
    for (unsigned i=0; i<sizeof fields/sizeof fields[0]; ++i) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(mqtt,fields[i].name);
        if (i==5 && (!value || (cJSON_IsString(value) && !value->valuestring[0]))) continue;
        if (!cJSON_IsString(value) || !value->valuestring[0] ||
            strlen(value->valuestring)>=fields[i].capacity) return false;
        if (!i && strpbrk(value->valuestring,"/\r\n")) return false;
    }
    return true;
}
static bool identity_matches(const cJSON *root, const char *mac, const char *uuid) {
    const cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"version"),
                *m=cJSON_GetObjectItemCaseSensitive(root,"mac"),
                *u=cJSON_GetObjectItemCaseSensitive(root,"uuid");
    return cJSON_IsNumber(version) && version->valuedouble==1 &&
           cJSON_IsString(m) && !strcmp(m->valuestring,mac) &&
           cJSON_IsString(u) && uuid[0] && !strcmp(u->valuestring,uuid);
}
static void discard(char *data, size_t size) {
    if (data) { memset(data,0,size); free(data); }
}
bool xz_cloud_cache_version_current(const cJSON *cached) {
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(cached, "reported_firmware");
    return cJSON_IsString(version) && !strcmp(version->valuestring, XZ_CONSOLE_VERSION);
}
cJSON *xz_cloud_cache_load(void) {
    char mac[18],uuid[37]; xz_identity(mac,uuid);
    if (!uuid[0]) return NULL;
    nvs_handle_t handle;
    if (nvs_open("xiaozhi",NVS_READONLY,&handle)!=ESP_OK) return NULL;
    size_t size=0,capacity=0;
    char *data=NULL; cJSON *root=NULL;
    if (nvs_get_blob(handle,cache_key,NULL,&size)==ESP_OK && size>1 && size<=CACHE_MAX) {
        capacity=size; data=malloc(capacity);
        if (data && nvs_get_blob(handle,cache_key,data,&size)==ESP_OK && size==capacity && data[size-1]==0 &&
            strlen(data)==size-1) root=cJSON_ParseWithOpts(data,NULL,true);
    }
    nvs_close(handle); discard(data,capacity);
    if (!identity_matches(root,mac,uuid) || !mqtt_valid(root)) { cJSON_Delete(root); return NULL; }
    return root;
}
bool xz_cloud_cache_save(const cJSON *cloud) {
    if (!mqtt_valid(cloud)) return false;
    char mac[18],uuid[37]; xz_identity(mac,uuid);
    if (!uuid[0]) return false;
    cJSON *root=cJSON_CreateObject(), *mqtt=NULL;
    char *data=NULL,*old=NULL;
    size_t size=0,old_size=0,old_capacity=0;
    bool ok=false;
    if (!root || !cJSON_AddNumberToObject(root,"version",1) ||
        !cJSON_AddStringToObject(root,"reported_firmware",XZ_CONSOLE_VERSION) ||
        !cJSON_AddStringToObject(root,"mac",mac) || !cJSON_AddStringToObject(root,"uuid",uuid) ||
        !(mqtt=cJSON_AddObjectToObject(root,"mqtt"))) goto done;
    const cJSON *source=cJSON_GetObjectItemCaseSensitive(cloud,"mqtt");
    for (unsigned i=0; i<sizeof fields/sizeof fields[0]; ++i) {
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(source,fields[i].name);
        if (cJSON_IsString(value) && value->valuestring[0] &&
            !cJSON_AddStringToObject(mqtt,fields[i].name,value->valuestring)) goto done;
    }
    data=cJSON_PrintUnformatted(root);
    if (!data) goto done;
    size=strlen(data)+1;
    if (size>CACHE_MAX) goto done;
    nvs_handle_t handle;
    if (nvs_open("xiaozhi",NVS_READWRITE,&handle)!=ESP_OK) goto done;
    // Unchanged credentials do not consume additional Flash writes.
    if (nvs_get_blob(handle,cache_key,NULL,&old_size)==ESP_OK && old_size==size) {
        old_capacity=old_size; old=malloc(old_capacity);
        if (old && nvs_get_blob(handle,cache_key,old,&old_size)==ESP_OK && old_size==size)
            ok=!memcmp(old,data,size);
    }
    if (!ok) ok=nvs_set_blob(handle,cache_key,data,size)==ESP_OK && nvs_commit(handle)==ESP_OK;
    nvs_close(handle);
done:
    discard(old,old_capacity); discard(data,size); cJSON_Delete(root);
    return ok;
}
bool xz_cloud_cache_clear(void) {
    nvs_handle_t handle;
    esp_err_t error=nvs_open("xiaozhi",NVS_READWRITE,&handle);
    if (error==ESP_ERR_NVS_NOT_FOUND) return true;
    if (error!=ESP_OK) return false;
    error=nvs_erase_key(handle,cache_key);
    bool ok=error==ESP_ERR_NVS_NOT_FOUND || (error==ESP_OK && nvs_commit(handle)==ESP_OK);
    nvs_close(handle);
    return ok;
}
