#include "presentation/companion_resources.h"
#include "companion_pack_meta.h"
#ifdef PASSPORT_MINIAPP
#include "presentation/compact_face.h"
#endif
#include "body_font_meta.h"
#include "presentation/hangul_font.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include <string.h>
static const uint8_t *mapped;
static esp_partition_mmap_handle_t handle;
static esp_partition_mmap_handle_t body_handle;
static lv_image_dsc_t frames[COMPANION_COUNT][COMPANION_FACE_COUNT];
static companion_portrait_t portraits[COMPANION_COUNT];
extern void passport_bind_body(const uint8_t *data);
extern void passport_bind_nickname(const uint8_t *data);
static const char fingerprint[] __attribute__((used)) =
    "COMPANION-SHA256:" COMPANION_SHA;
static const char body_fingerprint[] __attribute__((used)) = BODY_FONT_FINGERPRINT;
static const uint8_t body_header[64] = BODY_FONT_HEADER_BYTES;
static uint32_t u32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
bool companion_resources_init(void) {
  if (mapped)
    return true;
  const esp_partition_t *part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x41, "companion");
#ifdef PASSPORT_MINIAPP
  const unsigned format = 4;
  if (!part || part->size < COMPANION_SIZE || part->encrypted)
#else
  const unsigned format = 3;
  if (!part || part->address != 0x600000 || part->size != 0x200000 ||
      !part->readonly || part->encrypted)
#endif
    return false;
  const void *raw = NULL;
  if (esp_partition_mmap(part, 0, COMPANION_SIZE, ESP_PARTITION_MMAP_DATA, &raw,
                         &handle) != ESP_OK)
    return false;
  const uint8_t *p = raw;
  bool valid =
      !memcmp(p, "CPR1", 4) && u32(p + 4) == format &&
      u32(p + 8) == COMPANION_SIZE && u32(p + 12) == COMPANION_CRC &&
      esp_rom_crc32_le(0, p + 16, COMPANION_SIZE - 16) == COMPANION_CRC;
  uint32_t end = 128;
  for (unsigned i = 0; valid && i < 2 + COMPANION_COUNT * COMPANION_FACE_COUNT; i++) {
    uint32_t at = u32(p + 16 + i * 8), n = u32(p + 20 + i * 8);
    valid = at >= end && !(at % 4) && at <= COMPANION_SIZE && n &&
            n <= COMPANION_SIZE - at && (i < 2 || (format == 4 ? n >= 78 && !memcmp(p + at, "XZF1", 4) : n == COMPANION_FACE_WIDTH * COMPANION_FACE_HEIGHT * 3));
    end = at + n;
  }
  valid = valid && u32(p + 20) == 32;
  const esp_partition_t *body_part = valid ?
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "petart") : NULL;
  const void *body = NULL;
  if (!body_part ||
#ifndef PASSPORT_MINIAPP
      body_part->address != 0x500000 || !body_part->readonly ||
#endif
      body_part->size < BODY_FONT_OFFSET + BODY_FONT_SIZE || body_part->encrypted ||
      esp_partition_mmap(body_part, BODY_FONT_OFFSET, BODY_FONT_SIZE,
                         ESP_PARTITION_MMAP_DATA, &body, &body_handle) != ESP_OK) {
    valid = false;
  } else if (memcmp(body, body_header, sizeof body_header) ||
             esp_rom_crc32_le(0, (const uint8_t *)body + 16, BODY_FONT_SIZE - 16) != BODY_FONT_CRC) {
    esp_partition_munmap(body_handle);
    valid = false;
  }
  if (!valid) {
    esp_partition_munmap(handle);
    ESP_LOGE("companion", "Missing/mismatched resource pack");
    return false;
  }
  const uint8_t *body_bytes = body;
  passport_hangul_bind(body_bytes + u32(body_bytes + 60), BODY_FONT_SIZE - u32(body_bytes + 60),
                       p + HANGUL_TITLE_OFFSET, HANGUL_TITLE_BYTES);
  #ifdef ESP_PLATFORM
  extern const uint8_t lang_begin[] asm("_binary_languages_bin_start"), lang_end[] asm("_binary_languages_bin_end");
  passport_languages_bind(lang_begin,(size_t)(lang_end-lang_begin));
  #endif
  mapped = p;
  passport_bind_body(body);
  passport_bind_nickname(p + u32(p + 24));
  for (unsigned i = 0; i < COMPANION_COUNT; i++) {
    portraits[i].frames = frames[i];
    for (unsigned frame = 0; frame < COMPANION_FACE_COUNT; frame++) {
    frames[i][frame] = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                                               .cf = format == 4 ? LV_COLOR_FORMAT_RAW_ALPHA : LV_COLOR_FORMAT_RGB565A8,
                                               .flags = format == 4 ? LV_IMAGE_FLAGS_USER1 : 0,
                                               .w = COMPANION_FACE_WIDTH,
                                               .h = COMPANION_FACE_HEIGHT,
                                               .stride = COMPANION_FACE_WIDTH * 2},
                                    .data_size = u32(p + 36 + (i * COMPANION_FACE_COUNT + frame) * 8),
                                    .data = p + u32(p + 32 + (i * COMPANION_FACE_COUNT + frame) * 8)};
    }
  }
  ESP_LOGI("companion", "Resources mapped: %u bytes (%s)", COMPANION_SIZE,
           fingerprint + 17);
  ESP_LOGI("companion", "Body font mapped: %u bytes (%s)", BODY_FONT_SIZE, body_fingerprint + 16);
  return true;
}
const uint8_t *companion_official_icon(unsigned index) {
  return mapped && index < 6 ? mapped + OFFICIAL_ICONS_OFFSET + index * 3072 : NULL;
}
const uint8_t *companion_home_avatar(void) {
  return mapped ? mapped + DEFAULT_AVATAR_OFFSET : NULL;
}
const companion_portrait_t *companion_avatar(unsigned index) {
#ifdef PASSPORT_MINIAPP
  if (mapped && index < COMPANION_COUNT && !compact_face_register()) return NULL;
#endif
  return mapped && index < COMPANION_COUNT ? &portraits[index] : NULL;
}
