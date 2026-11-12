#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "xz_network.h"
#include "xz_theme.h"
#include "xz_theme_image.h"
#include "xz_theme_transfer.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

enum { CACHE_HEADER = 256, URL_MAX = 768, PIXEL_BLOCK = 1024 };
typedef struct {
  uint8_t first, count;
} frames_t;
static xz_theme_t installed;
static atomic_bool ready;
static bool boot_job;
static const uint8_t *cached;
static frames_t frames[XZ_EMOTIONS];
static unsigned cache_edge = XZ_ICON_EDGE;
static unsigned record_size(void) { return 4 + 3 * cache_edge * cache_edge; }
static atomic_bool dark = true, requested_reboot;
static const esp_partition_t *assets, *cache;
static esp_partition_mmap_handle_t asset_handle, cache_handle;
static const uint8_t *mapped_asset;
static const char *space = "xz_theme";
static const char *theme_error = "主题下载失败\n检查网络后重试";
const char *xz_theme_last_error(void) { return theme_error; }
static bool set_u8(const char *name, unsigned v) {
  nvs_handle_t h;
  if (nvs_open(space, NVS_READWRITE, &h) != ESP_OK)
    return false;
  bool ok = nvs_set_u8(h, name, v) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}
bool xz_theme_dark(void) { return atomic_load(&dark); }
bool xz_theme_set_dark(bool v) {
  if (xz_theme_dark() == v)
    return true;
  if (!set_u8("dark", v))
    return false;
  atomic_store(&dark, v);
  return true;
}
const xz_theme_t *xz_theme_installed(void) { return ready ? &installed : NULL; }
unsigned xz_theme_address(void) { return assets ? assets->address : 0; }
bool xz_theme_queue_reboot(void) {
  atomic_store(&requested_reboot, true);
  return true;
}
bool xz_theme_reboot_requested(void) { return atomic_load(&requested_reboot); }
bool xz_theme_queue_url(const char *url) {
  if (!assets || !url || strlen(url) > URL_MAX)
    return false;
  const char *host;
  if (!strncmp(url, "https://", 8))
    host = url + 8;
  else if (!strncmp(url, "http://", 7))
    host = url + 7;
  else
    return false;
  if (!*host || strchr("/?#:", *host))
    return false;
  for (const unsigned char *c = (const unsigned char *)url; *c; c++)
    if (*c <= 32 || *c == 127)
      return false;
  nvs_handle_t h;
  if (nvs_open(space, NVS_READWRITE, &h) != ESP_OK)
    return false;
  bool ok = nvs_set_str(h, "url", url) == ESP_OK && nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok;
}
static bool map_pack(xz_theme_t *result, const uint8_t **map, esp_partition_mmap_handle_t *handle,
                     size_t *used) {
  uint8_t h[12];
  if (!assets || esp_partition_read(assets, 0, h, sizeof h) != ESP_OK || !xz_u32(h) ||
      xz_u32(h) > 80 || xz_u32(h + 8) > assets->size - 12)
    return false;
  *used = 12 + xz_u32(h + 8);
  if (esp_partition_mmap(assets, 0, *used, ESP_PARTITION_MMAP_DATA, (const void **)map, handle) !=
      ESP_OK)
    return false;
  if (xz_theme_parse(*map, *used, result))
    return true;
  esp_partition_munmap(*handle);
  *map = NULL;
  return false;
}
typedef struct {
  unsigned next;
  uint8_t *block;
} frame_sink_t;
static bool write_frame(void *ctx, const xz_rgba_t *rgba, unsigned delay) {
  frame_sink_t *s = ctx;
  unsigned record = record_size(), pixels = cache_edge * cache_edge;
  unsigned at = CACHE_HEADER + s->next * record;
  if (at + record > cache->size)
    return false;
  uint8_t *p = s->block;
  p[0] = delay;
  p[1] = delay >> 8;
  p[2] = p[3] = 0;
  if (esp_partition_write(cache, at, p, 4) != ESP_OK)
    return false;
  for (unsigned i = 0; i < pixels; i += PIXEL_BLOCK) {
    for (unsigned j = 0; j < PIXEL_BLOCK; j++) {
      const xz_rgba_t *c = &rgba[i+j];
      unsigned rgb = ((c->r >> 3) << 11) | ((c->g >> 2) << 5) | (c->b >> 3);
      p[2*j] = rgb;
      p[2*j+1] = rgb >> 8;
    }
    if (esp_partition_write(cache, at + 4 + 2*i, p, 2*PIXEL_BLOCK) != ESP_OK)
      return false;
    for (unsigned j = 0; j < PIXEL_BLOCK; j++)
      p[j] = rgba[i+j].a;
    if (esp_partition_write(cache, at + 4 + 2*pixels + i, p, PIXEL_BLOCK) != ESP_OK)
      return false;
  }
  s->next++;
  return true;
}
typedef struct {
  uint32_t magic, source_crc, count, frame_crc, edge;
  frames_t lists[XZ_EMOTIONS];
} cache_header_t;
_Static_assert(sizeof(cache_header_t) <= CACHE_HEADER, "cache header");
static bool load_cache(uint32_t crc) {
  cache_header_t h;
  if (esp_partition_read(cache, 0, &h, sizeof h) != ESP_OK || h.magic != 0x32434d45 ||
      h.edge != cache_edge || h.source_crc != crc ||
      h.count > (cache->size - CACHE_HEADER) / record_size())
    return false;
  for (unsigned i = 0; i < XZ_EMOTIONS; i++)
    if (h.lists[i].first + h.lists[i].count > h.count)
      return false;
  size_t n = CACHE_HEADER + h.count * record_size();
  if (esp_partition_mmap(cache, 0, n, ESP_PARTITION_MMAP_DATA, (const void **)&cached,
                         &cache_handle) != ESP_OK)
    return false;
  if (esp_rom_crc32_le(0, cached + CACHE_HEADER, n - CACHE_HEADER) != h.frame_crc) {
    esp_partition_munmap(cache_handle);
    cached = NULL;
    return false;
  }
  memcpy(frames, h.lists, sizeof frames);
  return true;
}
static bool bake_cache(uint32_t crc) {
  unsigned left = 0;
  bool png_only = true, large = false;
  uint8_t owners[XZ_EMOTIONS];
  for (unsigned i = 0; i < XZ_EMOTIONS; i++) {
    owners[i] = i;
    if (!installed.icons[i].data)
      continue;
    // The official custom packer shares one file between multiple emotions.
    // Reuse its baked frames instead of decoding/writing the same PNG 21 times.
    for (unsigned j = 0; j < i; j++)
      if (installed.icons[i].data == installed.icons[j].data &&
          installed.icons[i].size == installed.icons[j].size) {
        owners[i] = j;
        break;
      }
    if (owners[i] == i) {
      left++;
      unsigned size = xz_theme_png_size(installed.icons[i]);
      png_only &= size != 0;
      large |= size > 64;
    }
  }
  if (!left)
    return true;
  // Keep GIF frame budgets intact. Static packs can use a sharper cache when
  // every distinct image fits; aliases share the same frame and resolution.
  cache_edge = png_only && large && left <= (cache->size - CACHE_HEADER) / (4 + 64*64*3)
                   ? 64 : XZ_ICON_EDGE;
  if (load_cache(crc))
    return true;
  if (esp_partition_erase_range(cache, 0, cache->size) != ESP_OK)
    return false;
  cache_header_t h = {.magic = 0x32434d45, .source_crc = crc, .edge = cache_edge};
  frame_sink_t s = {.block = malloc(2 * PIXEL_BLOCK)};
  if (!s.block)
    return false;
  bool ok = true;
  unsigned record = record_size(), total = (cache->size - CACHE_HEADER) / record;
  for (unsigned i = 0; i < XZ_EMOTIONS && ok; i++) {
    if (!installed.icons[i].data)
      continue;
    if (owners[i] != i) {
      h.lists[i] = h.lists[owners[i]];
      continue;
    }
    unsigned quota = total - s.next - (--left), first = s.next;
    if (quota > 12)
      quota = 12;
    ok = (cache_edge == 64 ? xz_theme_decode_png(installed.icons[i], 64, write_frame, &s)
                          : xz_theme_decode_image(installed.icons[i], quota, write_frame, &s)) &&
         s.next > first;
    h.lists[i] = (frames_t){first, s.next - first};
    vTaskDelay(1);
  }
  free(s.block);
  if (!ok)
    return false;
  h.count = s.next;
  const void *ptr;
  esp_partition_mmap_handle_t test;
  if (esp_partition_mmap(cache, CACHE_HEADER, h.count * record, ESP_PARTITION_MMAP_DATA, &ptr,
                         &test) != ESP_OK)
    return false;
  h.frame_crc = esp_rom_crc32_le(0, ptr, h.count * record);
  esp_partition_munmap(test);
  return esp_partition_write(cache, 0, &h, sizeof h) == ESP_OK && load_cache(crc);
}
const uint8_t *xz_theme_icon(unsigned emotion, unsigned tick, unsigned *delay) {
  if (!ready || !cached || emotion >= XZ_EMOTIONS)
    return NULL;
  frames_t f = frames[emotion];
  if (!f.count)
    f = frames[0];
  if (!f.count)
    return NULL;
  unsigned record = record_size();
  const uint8_t *p = cached + CACHE_HEADER + f.first * record;
  *delay = 0;
  if (f.count > 1) {
    unsigned duration = 0;
    for (unsigned j = 0; j < f.count; j++)
      duration += xz_u16(p + j * record);
    if (duration) {
      tick %= duration;
      for (unsigned j = 0; j < f.count; j++) {
        unsigned d = xz_u16(p);
        if (tick < d) {
          *delay = d - tick;
          break;
        }
        tick -= d;
        p += record;
      }
    }
  }
  return p + 4;
}
unsigned xz_theme_icon_edge(unsigned emotion, unsigned *display_edge) {
  *display_edge = XZ_ICON_EDGE;
  if (!ready || !cached || emotion >= XZ_EMOTIONS)
    return XZ_ICON_EDGE;
  if (!frames[emotion].count)
    emotion = 0;
  unsigned size = xz_theme_png_size(installed.icons[emotion]);
  if (size > 64)
    *display_edge = size < 128 ? size : 128;
  return cache_edge;
}
void xz_theme_boot(void) {
  static bool done;
  if (done)
    return;
  done = true;
  assets = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                    "assets");
  cache = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x42, "theme_cache");
  if (!assets || !cache ||
#ifndef PASSPORT_MINIAPP
      assets->address != 0x41a000 || cache->address != 0x5e9000 ||
#endif
      assets->size != XZ_THEME_LIMIT || assets->readonly || cache->size != XZ_CACHE_LIMIT ||
      cache->readonly) {
    assets = NULL;
    return;
  }
  nvs_handle_t h;
  if (nvs_open(space, NVS_READONLY, &h) == ESP_OK) {
    uint8_t d;
    size_t n = 0;
    if (nvs_get_u8(h, "dark", &d) == ESP_OK)
      atomic_store(&dark, d != 0);
    boot_job = nvs_get_str(h, "url", NULL, &n) == ESP_OK && n > 1 && n <= URL_MAX + 1;
    nvs_close(h);
  }
  if (boot_job)
    return; // Never write a partition currently feeding the display.
  size_t n;
  if (map_pack(&installed, &mapped_asset, &asset_handle, &n)) {
    ready = bake_cache(esp_rom_crc32_le(0, mapped_asset, n));
    if (!ready) {
      esp_partition_munmap(asset_handle);
      mapped_asset = NULL;
    }
  }
}
bool xz_theme_pending_boot(void) { return boot_job; }
void xz_theme_cancel_download(void) {
  boot_job = false;
  nvs_handle_t h;
  if (nvs_open(space, NVS_READWRITE, &h) == ESP_OK) {
    if (nvs_erase_key(h, "url") == ESP_OK)
      nvs_commit(h);
    nvs_close(h);
  }
}
typedef struct {
  esp_http_client_handle_t client;
  const atomic_bool *stop;
} http_transfer_t;
static int read_http(void *c, uint8_t *p, size_t n) {
  http_transfer_t *h = c;
  if (atomic_load(h->stop))
    return -1;
  return esp_http_client_read(h->client, (char *)p, n);
}
static bool erase_flash(void *c, size_t pos, size_t n) {
  (void)c;
  vTaskDelay(1);
  return esp_partition_erase_range(assets, pos, n) == ESP_OK;
}
static bool write_flash(void *c, size_t pos, const void *p, size_t n) {
  (void)c;
  return esp_partition_write(assets, pos, p, n) == ESP_OK;
}
static bool eof_http(void *c) {
  http_transfer_t *t = c;
  if (esp_http_client_is_complete_data_received(t->client))
    return true;
  uint8_t extra;
  return read_http(c, &extra, 1) == 0 && esp_http_client_is_complete_data_received(t->client);
}
bool xz_theme_download(const atomic_bool *stop) {
  theme_error = "主题下载失败\n检查网络后重试";
  if (!boot_job || mapped_asset)
    return false;
  boot_job = false;
  char *url = malloc(URL_MAX + 1);
  nvs_handle_t h;
  if (!url || nvs_open(space, NVS_READWRITE, &h) != ESP_OK) {
    free(url);
    return false;
  }
  size_t len = URL_MAX + 1;
  bool ok = nvs_get_str(h, "url", url, &len) == ESP_OK && nvs_erase_key(h, "url") == ESP_OK &&
            nvs_commit(h) == ESP_OK;
  nvs_close(h);
  if (!ok) {
    free(url);
    return false;
  }
  esp_http_client_config_t cfg = {.url = url,
                                  .crt_bundle_attach = esp_crt_bundle_attach,
                                  .timeout_ms = 8000,
                                  .buffer_size = 1024,
                                  .buffer_size_tx = 768,
                                  .disable_auto_redirect = true};
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  ok = false;
  if (client) {
    for (unsigned attempt = 0; attempt < 4 && !atomic_load(stop); attempt++) {
      if (esp_http_client_open(client, 0) != ESP_OK)
        break;
      int64_t length = esp_http_client_fetch_headers(client);
      int code = esp_http_client_get_status_code(client);
      if (code == 200) {
        if (length > assets->size) {
          theme_error = "主题包过大\n请控制在920KiB内";
          break;
        }
        http_transfer_t ctx = {client, stop};
        const xz_transfer_t io = {read_http, erase_flash, write_flash, eof_http};
        ok = xz_theme_receive(&io, &ctx, length, assets->size);
        break;
      }
      if (code < 300 || code >= 400 || esp_http_client_set_redirection(client) != ESP_OK)
        break;
      esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
  }
  free(url);
  if (ok) {
    theme_error = "主题加载失败\n图片建议128×128以内";
    size_t n;
    ok = map_pack(&installed, &mapped_asset, &asset_handle, &n);
    if (ok) {
      ok = bake_cache(esp_rom_crc32_le(0, mapped_asset, n));
      if (cached) {
        esp_partition_munmap(cache_handle);
        cached = NULL;
      }
      esp_partition_munmap(asset_handle);
      mapped_asset = NULL;
    }
    if (!ok)
      (void)esp_partition_erase_range(assets, 0, 4096);
  }
  return ok;
}
