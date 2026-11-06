#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_MMAP_DATA 0
typedef unsigned esp_partition_mmap_handle_t;
typedef struct {uint32_t address,size;bool readonly,encrypted;} esp_partition_t;
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *name);
int esp_partition_mmap(const esp_partition_t *part,size_t offset,size_t size,int type,
                      const void **out,esp_partition_mmap_handle_t *handle);
void esp_partition_munmap(esp_partition_mmap_handle_t handle);
