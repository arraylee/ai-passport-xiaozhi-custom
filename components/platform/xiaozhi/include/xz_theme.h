#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum {
  XZ_OFFICIAL_AVATAR = 2,
  XZ_AVATARS = 3,
  XZ_EMOTIONS = 21,
  XZ_ICON_EDGE = 32,
  XZ_ICON_BYTES = 32 * 32 * 3,
  XZ_THEME_LIMIT = 0xe6000,
  XZ_CACHE_LIMIT = 0x17000
};
typedef struct {
  const uint8_t *data;
  size_t size;
} xz_blob_t;
typedef struct {
  uint32_t ink, background;
  xz_blob_t image;
} xz_skin_t;
typedef struct {
  xz_skin_t skin[2];
  xz_blob_t font, icons[XZ_EMOTIONS];
  bool hide_subtitle;
} xz_theme_t;
static inline unsigned xz_u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static inline uint32_t xz_u32(const uint8_t *p) {
  return xz_u16(p) | (uint32_t)xz_u16(p + 2) << 16;
}
int xz_emotion_index(const char *name);
bool xz_theme_parse(const uint8_t *data, size_t size, xz_theme_t *out);
const xz_theme_t *xz_theme_installed(void);
const uint8_t *xz_theme_icon(unsigned emotion, unsigned tick, unsigned *delay);
bool xz_theme_dark(void);
bool xz_theme_set_dark(bool dark);
bool xz_theme_queue_url(const char *url);
bool xz_theme_queue_reboot(void);
unsigned xz_theme_address(void);
#include <stdatomic.h>
void xz_theme_boot(void);
bool xz_theme_pending_boot(void);
void xz_theme_cancel_download(void);
bool xz_theme_download(const atomic_bool *stop);
bool xz_theme_reboot_requested(void);
