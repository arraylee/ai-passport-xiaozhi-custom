/* Compile this fixture with xz_mcp, xz_theme*, xz_protocol, cJSON and puff.
 * The two inputs are actual assets.bin files from the XiaoZhi packer. */
#include "xz_mcp.h"
#include "xz_protocol.h"
#include "xz_theme.h"
#include "xz_theme_font.h"
#include "xz_theme_image.h"
#include "xz_theme_transfer.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned volume = 95, brightness = 80, calls, frames;
static bool fail;
static unsigned vol(void *p) {
  (void)p;
  return volume;
}
static unsigned light(void *p) {
  (void)p;
  return brightness;
}
static bool set_vol(void *p, unsigned v) {
  (void)p;
  calls++;
  if (!fail)
    volume = v;
  return !fail;
}
static bool set_light(void *p, unsigned v) {
  (void)p;
  calls++;
  if (!fail)
    brightness = v;
  return !fail;
}
static bool url(const char *s) { return !strcmp(s, "https://example.org/theme.bin"); }
static bool restart(void) { return true; }
static bool set_dark(bool dark) { return dark; }
static unsigned count;
static cJSON *rpc(const char *method, const char *params) {
  char query[768];
  snprintf(query, sizeof query, "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"%s\",\"params\":%s}",
           ++count, method, params);
  cJSON *req = cJSON_Parse(query);
  assert(req);
  xz_config_t config = {.get_volume = vol,
                        .set_volume = set_vol,
                        .get_brightness = light,
                        .set_brightness = set_light};
  xz_mcp_device_t device = {url, restart, set_dark, "2.1.0-xiaozhi.15", 0x41a000, XZ_THEME_LIMIT};
  char *raw = xz_mcp_reply(req, "session-123", &config, &device);
  assert(raw);
  cJSON *wire = cJSON_Parse(raw);
  free(raw);
  cJSON_Delete(req);
  assert(!strcmp(cJSON_GetObjectItem(wire, "type")->valuestring, "mcp"));
  assert(!strcmp(cJSON_GetObjectItem(wire, "session_id")->valuestring, "session-123"));
  cJSON *msg = cJSON_DetachItemFromObject(wire, "payload");
  cJSON_Delete(wire);
  assert(cJSON_GetObjectItem(msg, "id")->valueint == (int)count);
  return msg;
}
static void call(const char *name, const char *args, bool error, const char *text) {
  char p[512];
  snprintf(p, sizeof p, "{\"name\":\"%s\",\"arguments\":%s}", name, args);
  cJSON *m = rpc("tools/call", p), *r = cJSON_GetObjectItem(m, "result");
  assert(r);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(r, "isError")) == error);
  cJSON *item = cJSON_GetArrayItem(cJSON_GetObjectItem(r, "content"), 0);
  assert(strstr(cJSON_GetObjectItem(item, "text")->valuestring, text));
  cJSON_Delete(m);
}
static void controls(void) {
  cJSON *m = rpc("initialize", "{}");
  assert(
      !strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(m, "result"), "protocolVersion")->valuestring,
              "2024-11-05"));
  cJSON_Delete(m);
  for (unsigned i = 0; i < 100; i++) {
    bool user = i % 2;
    m = rpc("tools/list", user ? "{\"withUserTools\":true}" : "{}");
    cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(m, "result"), "tools");
    assert(cJSON_GetArraySize(tools) == (user ? 11 : 7));
    bool saw = false;
    for (int j = 0; j < cJSON_GetArraySize(tools); j++) {
      cJSON *t = cJSON_GetArrayItem(tools, j);
      if (!strcmp(cJSON_GetObjectItem(t, "name")->valuestring, "self.assets.set_download_url")) {
        saw = true;
        assert(cJSON_GetObjectItem(t, "annotations"));
      }
    }
    assert(saw == user);
    cJSON_Delete(m);
  }
  call("self.get_device_status", "{}", false, "\"volume\":95");
  call("self.audio_speaker.set_volume", "{\"volume\":60}", false, "true");
  call("self.screen.set_brightness", "{\"brightness\":90}", false, "true");
  call("self.get_device_status", "{}", false, "\"brightness\":90");
  call("self.audio_speaker.set_volume", "{\"volume\":101}", true, "Invalid");
  call("self.screen.set_brightness", "{\"brightness\":0}", true, "Invalid");
  call("self.screen.set_brightness", "{\"brightness\":45.5}", true, "Invalid");
  assert(calls == 2 && volume == 60 && brightness == 90);
  fail = true;
  call("self.audio_speaker.set_volume", "{\"volume\":10}", true, "Invalid");
  assert(volume == 60);
  call("self.screen.get_info", "{}", false, "\"width\":240");
  call("self.get_system_info", "{}", false, "\"label\":\"assets\"");
  call("self.assets.set_download_url", "{\"url\":\"https://example.org/theme.bin\"}", false,
       "true");
  call("self.reboot", "{}", false, "true");
  printf("MCP hello negotiation, discovery, user-only list, absolute/relative status and persisted "
         "setter errors: PASS\n");
}
typedef struct {
  uint8_t *bank, *source;
  size_t size, available, pos;
  bool extra;
  unsigned erases;
} remote_t;
static int read_net(void *c, uint8_t *p, size_t n) {
  remote_t *r = c;
  if (r->pos == r->available)
    return 0;
  if (n > r->available - r->pos)
    n = r->available - r->pos;
  if (n > 317)
    n = 317;
  memcpy(p, r->source + r->pos, n);
  r->pos += n;
  return n;
}
static bool erase(void *c, size_t pos, size_t n) {
  remote_t *r = c;
  assert(!(pos % 4096) && n == 4096 && pos + n <= XZ_THEME_LIMIT);
  memset(r->bank + pos, 255, n);
  r->erases++;
  return true;
}
static bool write_flash(void *c, size_t pos, const void *p, size_t n) {
  remote_t *r = c;
  assert(pos + n <= XZ_THEME_LIMIT);
  memcpy(r->bank + pos, p, n);
  return true;
}
static bool complete(void *c) {
  remote_t *r = c;
  return !r->extra && r->pos == r->size;
}
static xz_transfer_t io = {read_net, erase, write_flash, complete};
static bool frame(void *ctx, const xz_rgba_t *p, unsigned delay) {
  (void)ctx;
  (void)delay;
  unsigned ink = 0;
  for (unsigned i = 0; i < 1024; i++)
    ink += (p[i].a != 0);
  assert(ink > 20);
  frames++;
  return true;
}
static void check_file(const char *name) {
  FILE *fp = fopen(name, "rb");
  assert(fp);
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  rewind(fp);
  assert(n > 44 && n <= XZ_THEME_LIMIT);
  uint8_t *p = malloc(n), *guard = malloc(XZ_THEME_LIMIT + 128);
  assert(p && guard && fread(p, 1, n, fp) == (size_t)n);
  fclose(fp);
  memset(guard, 0xa6, XZ_THEME_LIMIT + 128);
  remote_t r = {.bank = guard + 64, .source = p, .size = n, .available = n};
  assert(xz_theme_receive(&io, &r, -1, XZ_THEME_LIMIT));
  assert(!memcmp(p, r.bank, n));
  for (unsigned i = 0; i < 64; i++)
    assert(guard[i] == 0xa6 && guard[XZ_THEME_LIMIT + 64 + i] == 0xa6);
  xz_theme_t theme = {0};
  assert(xz_theme_parse(r.bank, n, &theme));
  if (theme.font.data) {
    xz_theme_font_t f;
    assert(xz_theme_font_open(theme.font, &f));
    xz_theme_glyph_t g;
    uint8_t pixels[128 * 128];
    unsigned matches = 0;
    for (unsigned cp = 0; cp < 65536; cp++)
      if (xz_theme_glyph(&f, cp, &g)) {
        assert(xz_theme_pixels(&f, &g, pixels, 128));
        matches++;
      }
    assert(matches > 5000);
    printf("Official font: %u glyphs decoded safely, %ubpp\n", matches, f.bpp);
  }
  for (unsigned i = 0; i < XZ_EMOTIONS; i++)
    if (theme.icons[i].data)
      assert(xz_theme_decode_image(theme.icons[i], 12, frame, NULL));
  assert(!xz_theme_parse(r.bank, n - 1, &theme));
  r.bank[n - 1] ^= 31;
  assert(!xz_theme_parse(r.bank, n, &theme));
  for (size_t k = 15; k < (size_t)n; k += 33217) {
    memset(r.bank, 0xff, XZ_THEME_LIMIT);
    r.pos = 0;
    r.available = k;
    assert(!xz_theme_receive(&io, &r, n, XZ_THEME_LIMIT));
    assert(xz_u32(r.bank) == 0xffffffffu);
  }
  r.pos = 0;
  r.available = n;
  r.extra = true;
  assert(!xz_theme_receive(&io, &r, -1, XZ_THEME_LIMIT));
  assert(xz_u32(r.bank) == 0xffffffffu);
  r.pos = 0;
  r.extra = false;
  assert(xz_theme_receive(&io, &r, n, XZ_THEME_LIMIT));
  assert(!memcmp(p, r.bank, n));
  r.pos = 0;
  r.erases = 0;
  assert(!xz_theme_receive(&io, &r, n + 1, XZ_THEME_LIMIT) && !r.erases);
  free(guard);
  free(p);
}
int main(int argc, char **argv) {
  assert(argc >= 2);
  controls();
  char s[80];
  xz_text_copy(s, sizeof s, "한국어 · Tôi · ば");
  assert(!strcmp(s, "한국어 · Tôi · ば"));
  for (int i = 1; i < argc; i++)
    check_file(argv[i]);
  printf(
      "Isolated theme writes, bounds, fetch failure and retry; %u PNG/GIF frames decoded: PASS\n",
      frames);
  return 0;
}
