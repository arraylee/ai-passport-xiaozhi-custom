#include "presentation/pet_art_pack.h"
#include "presentation/pixel_sprite.h"
#include <string.h>

static uint16_t read16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t read32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t crc32(const uint8_t *p, size_t size) {
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
void pet_art_pack_unbind(const pet_art_asset_t *catalog, size_t count) {
    if (!catalog || count > PET_ART_PACK_MAX_COUNT) return;
    for (size_t i = 0; i < count; ++i) if (catalog[i].image) {
        catalog[i].image->data = NULL;
        catalog[i].image->data_size = 0;
    }
}
bool pet_art_pack_bind(const void *data, size_t size, const pet_art_asset_t *catalog,
                       size_t count, const char *fingerprint) {
    pet_art_pack_unbind(catalog, count);
    if (!data || !catalog || !count || count > PET_ART_PACK_MAX_COUNT ||
        size < 64 || size > PET_ART_PACK_CAPACITY || !fingerprint ||
        strncmp(fingerprint, "PETART-SHA256:", 14) || strlen(fingerprint) != 78) return false;
    const uint8_t *p = data;
    if (memcmp(p, "PTP1", 4) || read16(p+4) != 1 || read16(p+6) != 64 ||
        read32(p+8) != size || read32(p+12) != count || read32(p+16) != 24 ||
        read32(p+20) != 64) return false;
    for (unsigned i = 36; i < 64; ++i) if (p[i]) return false;
    size_t base = 64 + count * 24;
    if (base >= size || read32(p+24) != base ||
        crc32(p+64, base-64) != read32(p+28) || crc32(p+base, size-base) != read32(p+32)) return false;
    size_t end = base;
    uint32_t previous_id = 0;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *entry = p+64+i*24;
        const pet_art_asset_t *expected = catalog+i;
        uint32_t id = read32(entry), offset = read32(entry+8), length = read32(entry+12);
        if ((i && id <= previous_id) || id != expected->id || !expected->image ||
            read16(entry+4) != expected->width || read16(entry+6) != expected->height ||
            length != expected->size || read32(entry+16) != expected->crc32 || read32(entry+20) ||
            offset != end || offset > size || !length || length > size-offset ||
            crc32(p+offset, length) != expected->crc32) return false;
        lv_image_dsc_t image = {.header = {.magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = PIXEL_SPRITE_FLAG,
            .w = expected->width, .h = expected->height}, .data_size = length, .data = p+offset};
        if (!pixel_sprite_validate(&image)) return false;
        end = (size_t)offset + length;
        previous_id = id;
    }
    if (end != size) return false;
    /* No earlier descriptor can be observed with data after a later failure. */
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *entry = p+64+i*24;
        lv_image_dsc_t *image = catalog[i].image;
        image->header = (lv_image_header_t){.magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = PIXEL_SPRITE_FLAG,
            .w = catalog[i].width, .h = catalog[i].height};
        image->data_size = catalog[i].size;
        image->data = p + read32(entry+8);
    }
    return true;
}
