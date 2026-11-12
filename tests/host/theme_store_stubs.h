/* Minimal host boundary for the production theme store. */

#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 130
#define ESP_PARTITION_MMAP_DATA 0
#define NVS_READWRITE 1
#define NVS_READONLY 0
typedef struct {unsigned address,size;bool readonly;uint8_t *bytes;} esp_partition_t;
typedef unsigned esp_partition_mmap_handle_t;
typedef int nvs_handle_t;
typedef void *esp_http_client_handle_t;
typedef struct {const char *url;int(*crt_bundle_attach)(void*);int timeout_ms,buffer_size,buffer_size_tx;bool disable_auto_redirect;} esp_http_client_config_t;
int esp_crt_bundle_attach(void*);
const esp_partition_t *esp_partition_find_first(int,int,const char*);
int esp_partition_read(const esp_partition_t*,size_t,void*,size_t);
int esp_partition_mmap(const esp_partition_t*,size_t,size_t,int,const void**,unsigned*);
void esp_partition_munmap(unsigned);
int esp_partition_write(const esp_partition_t*,size_t,const void*,size_t);
int esp_partition_erase_range(const esp_partition_t*,size_t,size_t);
unsigned esp_rom_crc32_le(unsigned,const void*,size_t);
int nvs_open(const char*,int,int*);void nvs_close(int);int nvs_commit(int);
int nvs_get_u8(int,const char*,uint8_t*);int nvs_set_u8(int,const char*,uint8_t);
int nvs_get_str(int,const char*,char*,size_t*);int nvs_set_str(int,const char*,const char*);int nvs_erase_key(int,const char*);
void vTaskDelay(int);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t*);
int esp_http_client_open(esp_http_client_handle_t,int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
int esp_http_client_set_redirection(esp_http_client_handle_t);
int esp_http_client_close(esp_http_client_handle_t);
int esp_http_client_cleanup(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t,char*,size_t);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
