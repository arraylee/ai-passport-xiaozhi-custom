#include "xz_theme.h"
#include "cJSON.h"
#include "xz_theme_font.h"
#include <string.h>

static const char *const emotions[XZ_EMOTIONS] = {
    "neutral", "happy",       "laughing",  "funny",     "sad",      "angry",   "crying",
    "loving",  "embarrassed", "surprised", "shocked",   "thinking", "winking", "cool",
    "relaxed", "delicious",   "kissy",     "confident", "sleepy",   "silly",   "confused"};
int xz_emotion_index(const char *s) {
  if (s)
    for (unsigned i = 0; i < XZ_EMOTIONS; i++)
      if (!strcmp(emotions[i], s))
        return i;
  return -1;
}
static bool json_depth_ok(xz_blob_t b) {
  unsigned depth = 0;
  bool quote = false;
  for (size_t i = 0; i < b.size; i++) {
    uint8_t c = b.data[i];
    if (quote && c == '\\') {
      if (++i == b.size)
        return false;
      continue;
    }
    if (c == '"')
      quote = !quote;
    if (!quote && (c == '{' || c == '[') && ++depth > 12)
      return false;
    if (!quote && (c == '}' || c == ']') && !depth--)
      return false;
    if (!c)
      return false;
  }
  return !quote && !depth;
}
static xz_blob_t asset(const uint8_t *p, unsigned count, const char *name) {
  if (!name || !name[0] || strlen(name) > 32)
    return (xz_blob_t){0};
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *entry = p + 12 + i * 44;
    if (!strncmp((const char *)entry, name, 32))
      return (xz_blob_t){p + 12 + count * 44 + xz_u32(entry + 36) + 2, xz_u32(entry + 32)};
  }
  return (xz_blob_t){0};
}
static const char *field(const cJSON *o, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool color(const cJSON *o, const char *key, uint32_t *out) {
  const char *s = field(o, key);
  if (!s)
    return true;
  if (strlen(s) != 7 || *s++ != '#')
    return false;
  uint32_t value = 0;
  while (*s) {
    unsigned c = (unsigned char)*s++;
    unsigned digit = c >= '0' && c <= '9'                 ? c - '0'
                     : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10
                                                          : 16;
    if (digit > 15)
      return false;
    value = (value << 4) | digit;
  }
  *out = value;
  return true;
}
bool xz_theme_parse(const uint8_t *p, size_t n, xz_theme_t *out) {
  if (!p || !out || n < 56 || n > XZ_THEME_LIMIT || xz_u32(p + 8) != n - 12)
    return false;
  unsigned count = xz_u32(p);
  if (!count || count > 80 || 12 + count * 44 > n)
    return false;
  uint32_t sum = 0;
  for (size_t i = 12; i < n; i++)
    sum += p[i];
  if ((sum & 65535) != xz_u32(p + 4))
    return false;
  size_t base = 12 + count * 44, end = 0;
  for (unsigned i = 0; i < count; i++) {
    const uint8_t *e = p + 12 + 44 * i;
    unsigned len = xz_u32(e + 32), pos = xz_u32(e + 36);
    if (!e[0] || pos != end || pos > n - base || n - base - pos < 2 || len > n - base - pos - 2 ||
        p[base + pos] != 0x5a || p[base + pos + 1] != 0x5a)
      return false;
    for (unsigned j = 0; j < i; j++)
      if (!strncmp((const char *)e, (const char *)(p + 12 + j * 44), 32))
        return false;
    end = pos + 2 + len;
  }
  xz_blob_t index = asset(p, count, "index.json");
  if (end != n - base || !index.size || index.size > 8192 || !json_depth_ok(index))
    return false;
  const char *parsed_end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts((char *)index.data, index.size, &parsed_end, false);
  bool ok = cJSON_IsObject(root);
  while (parsed_end && parsed_end < (const char *)index.data + index.size &&
         (unsigned char)*parsed_end <= 32)
    parsed_end++;
  ok = ok && parsed_end == (const char *)index.data + index.size;
  cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version"),
        *display = cJSON_GetObjectItemCaseSensitive(root, "display_config");
  const char *chip = field(root, "chip_model");
  if ((version && (!cJSON_IsNumber(version) || version->valuedouble != 1)) ||
      (chip && strcmp(chip, "esp32c3")))
    ok = false;
  if (display) {
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(display, "width"),
                *h = cJSON_GetObjectItemCaseSensitive(display, "height");
    if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h) || w->valuedouble != 240 ||
        h->valuedouble != 320 ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(display, "monochrome")))
      ok = false;
    const char *pixel_format = field(display, "color");
    if (pixel_format && strcmp(pixel_format, "RGB565"))
      ok = false;
  }
  memset(out, 0, sizeof *out);
  out->hide_subtitle = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "hide_subtitle"));
  const cJSON *skins = cJSON_GetObjectItemCaseSensitive(root, "skin");
  for (unsigned mode = 0; mode < 2 && ok; mode++) {
    xz_skin_t *s = &out->skin[mode];
    s->background = mode ? 0x121212 : 0xffffff;
    s->ink = mode ? 0xffffff : 0x151515;
    const cJSON *skin = cJSON_GetObjectItemCaseSensitive(skins, mode ? "dark" : "light");
    ok = color(skin, "background_color", &s->background) && color(skin, "text_color", &s->ink);
    const char *name = field(skin, "background_image");
    if (name) {
      xz_blob_t b = asset(p, count, name);
      const uint8_t *q = b.data;
      if (b.size < 28 || xz_u32(q) != 0x1219 || xz_u16(q + 4) != 240 || xz_u16(q + 6) != 320 ||
          xz_u32(q + 8) != 480 || xz_u32(q + 12) != 153600 || xz_u32(q + 16) > b.size ||
          xz_u32(q + 16) < 28 || b.size - xz_u32(q + 16) < 153600)
        ok = false;
      else
        s->image = (xz_blob_t){q + xz_u32(q + 16), 153600};
    }
  }
  const char *font = field(root, "text_font");
  if (font) {
    out->font = asset(p, count, font);
    xz_theme_font_t parsed;
    if (!xz_theme_font_open(out->font, &parsed))
      ok = false;
  }
  const cJSON *collection = cJSON_GetObjectItemCaseSensitive(root, "emoji_collection"),
              *emoji = NULL;
  if (collection && !cJSON_IsArray(collection))
    ok = false;
  cJSON_ArrayForEach(emoji, collection) {
    int i = xz_emotion_index(field(emoji, "name"));
    if (i < 0)
      continue;
    const char *file = field(emoji, "file");
    if (!file)
      continue;
    out->icons[i] = asset(p, count, file);
    if (!out->icons[i].size)
      ok = false;
  }
  cJSON_Delete(root);
  return ok;
}
