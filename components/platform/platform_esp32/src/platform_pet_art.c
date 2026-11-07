#include "platform/platform_pet_art.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "pet_art";
static const void *mapped;
static size_t mapped_size;
static esp_partition_mmap_handle_t handle;

bool platform_pet_art_map(const void **data, size_t *size) {
    if (!data || !size) return false;
    *data = NULL; *size = 0;
    if (mapped) { *data = mapped; *size = mapped_size; return true; }
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "petart");
    if (!part || part->address != 0x500000 || part->size != 0xe9000 || part->encrypted || !part->readonly) {
        ESP_LOGW(TAG, "Missing or incompatible petart partition"); return false;
    }
    uint8_t header[64];
    if (esp_partition_read(part, 0, header, sizeof(header)) != ESP_OK || memcmp(header, "PTP1", 4)) {
        ESP_LOGW(TAG, "Missing petart header"); return false;
    }
    uint32_t bytes = header[8] | (uint32_t)header[9] << 8 | (uint32_t)header[10] << 16 | (uint32_t)header[11] << 24;
    if (bytes < sizeof(header) || bytes > part->size) {
        ESP_LOGW(TAG, "Petart length outside partition"); return false;
    }
    if (esp_partition_mmap(part, 0, bytes, ESP_PARTITION_MMAP_DATA, &mapped, &handle) != ESP_OK) {
        mapped = NULL; ESP_LOGW(TAG, "Petart read-only map failed"); return false;
    }
    mapped_size = bytes;
    *data = mapped; *size = bytes;
    ESP_LOGI(TAG, "Mapped %u petart bytes from 0x500000", (unsigned)bytes);
    return true;
}
