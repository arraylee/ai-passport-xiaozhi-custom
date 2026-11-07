#include "presentation/hangul_font.h"
#include "../vendor/puff/puff.h"
#include <string.h>

enum { HEADER_BYTES = 24, SYLLABLES = 11172, LETTERS = 94 };
typedef struct {
  const uint8_t *data;
  size_t size;
  const uint8_t *codes, *advances;
} ko_data_t;
static ko_data_t supplemental;
LV_FONT_DECLARE(passport_lang_16);
LV_FONT_DECLARE(passport_lang_20);
static ko_data_t body, title;
static uint16_t le16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | (uint32_t)le16(p + 2) << 16; }

static ko_data_t bind_font(const uint8_t *p, size_t n, unsigned px, unsigned count) {
  if (!p || n < HEADER_BYTES || memcmp(p, "KOR1", 4) || le32(p + 4) != count || !le16(p + 8) ||
      le16(p + 8) > 24 || !le16(p + 10) || le16(p + 10) > 24 || !le16(p + 16) || le16(p + 18) != px)
    return (ko_data_t){0};
  unsigned group = le16(p + 16), blocks = le32(p + 20);
  if (blocks != (count + group - 1) / group || HEADER_BYTES + 4 * (blocks + 1) > n ||
      group * ((le16(p + 8) + 7) / 8) * le16(p + 10) > 512 ||
      le32(p + HEADER_BYTES) != HEADER_BYTES + 4 * (blocks + 1) ||
      le32(p + HEADER_BYTES + 4 * blocks) != n)
    return (ko_data_t){0};
  return (ko_data_t){.data = p, .size = n};
}
void passport_hangul_bind(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
  body = bind_font(a, an, 16, SYLLABLES + LETTERS);
  title = bind_font(b, bn, 20, SYLLABLES + LETTERS);
}
void passport_languages_bind(const uint8_t *p, size_t n) {
  supplemental = (ko_data_t){0};
  if (!p || n < 16 || memcmp(p, "LNG1", 4))
    return;
  unsigned count = le32(p + 4), at = le32(p + 8);
  if (!count || count > 8192 || at != 16 + 2 * count || at + count >= n || le32(p + 12) != n)
    return;
  supplemental = bind_font(p + at + count, n - at - count, 16, count);
  supplemental.codes = p + 16;
  supplemental.advances = p + at;
}
static int scale(int v, bool big) {
  return big ? (v >= 0 ? (v * 5 + 2) / 4 : -((-v * 5 + 2) / 4)) : v;
}
static bool lookup(const lv_font_t *font, lv_font_glyph_dsc_t *g, uint32_t cp, uint32_t next) {
  (void)next;
  const uint8_t *p = ((const ko_data_t *)font->dsc)->data;
  if (!p)
    return false;
  unsigned index;
  const ko_data_t *source = font->dsc;
  bool big = font == &passport_lang_20;
  unsigned advance = le16(p + 18);
  if (source->codes) {
    unsigned count = le32(p + 4), lo = 0, hi = count;
    while (lo < hi) {
      unsigned mid = (lo + hi) / 2;
      if (le16(source->codes + 2 * mid) < cp)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo >= count || le16(source->codes + 2 * lo) != cp)
      return false;
    index = lo;
    advance = source->advances[lo];
  } else {
    // Isolated modern conjoining jamo use their spacing compatibility form.
    static const uint8_t initial[] = {0,  1,  3,  6,  7,  8,  16, 17, 18, 20,
                                      21, 22, 23, 24, 25, 26, 27, 28, 29};
    static const uint8_t final[] = {0,  1,  2,  3,  4,  5,  6,  8,  9,  10, 11, 12, 13, 14,
                                    15, 16, 17, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29};
    if (cp >= 0x1100 && cp <= 0x1112)
      cp = 0x3131 + initial[cp - 0x1100];
    else if (cp >= 0x1161 && cp <= 0x1175)
      cp = 0x314f + cp - 0x1161;
    else if (cp >= 0x11a8 && cp <= 0x11c2)
      cp = 0x3131 + final[cp - 0x11a8];
    if (cp >= 0xac00 && cp <= 0xd7a3)
      index = cp - 0xac00;
    else if (cp >= 0x3131 && cp <= 0x318e)
      index = SYLLABLES + cp - 0x3131;
    else
      return false;
  }
  g->gid.index = index;
  g->adv_w = scale(advance, big);
  g->box_w = scale(le16(p + 8), big);
  g->box_h = scale(le16(p + 10), big);
  g->ofs_x = scale((int16_t)le16(p + 12), big);
  g->ofs_y = scale((int16_t)le16(p + 14), big);
  g->format = LV_FONT_GLYPH_FORMAT_A1;
  g->is_placeholder = false;
  return true;
}
static const void *bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *buf) {
  if (!g || !g->resolved_font || g->req_raw_bitmap || !buf || !buf->data ||
      buf->header.cf != LV_COLOR_FORMAT_A8)
    return NULL;
  const ko_data_t *src = g->resolved_font->dsc;
  const uint8_t *p = src->data;
  if (!p || g->gid.index >= le32(p + 4) || buf->header.w < g->box_w || buf->header.h < g->box_h ||
      buf->header.stride < g->box_w || buf->data_size / buf->header.stride < g->box_h)
    return NULL;
  unsigned w = le16(p + 8), h = le16(p + 10), group = le16(p + 16), block = g->gid.index / group,
           pitch = (w + 7) / 8;
  unsigned bytes = pitch * h, count = le32(p + 4) - block * group;
  if (count > group)
    count = group;
  const uint8_t *entry = p + HEADER_BYTES + 4 * block;
  unsigned begin = le32(entry), end = le32(entry + 4);
  if (end <= begin || end > src->size || begin < HEADER_BYTES + 4 * (le32(p + 20) + 1))
    return NULL;
  unsigned long in = end - begin, out = bytes * count;
  uint8_t scratch[512];
  if (out > sizeof scratch || puff(scratch, &out, p + begin, &in) || out != bytes * count ||
      in != end - begin)
    return NULL;
  const uint8_t *tile = scratch + (g->gid.index % group) * bytes;
  for (unsigned y = 0; y < g->box_h; y++) {
    unsigned sy = y * h / g->box_h;
    for (unsigned x = 0; x < g->box_w; x++) {
      unsigned sx = x * w / g->box_w;
      buf->data[y * buf->header.stride + x] =
          (tile[sy * pitch + sx / 8] & (128u >> (sx % 8))) ? 255 : 0;
    }
  }
  return buf;
}
const lv_font_t passport_ko_16 = {.get_glyph_dsc = lookup,
                                  .get_glyph_bitmap = bitmap,
                                  .line_height = 19,
                                  .base_line = 4,
                                  .dsc = &body,
                                  .fallback = &passport_lang_16};
const lv_font_t passport_ko_20 = {.get_glyph_dsc = lookup,
                                  .get_glyph_bitmap = bitmap,
                                  .line_height = 22,
                                  .base_line = 4,
                                  .dsc = &title,
                                  .fallback = &passport_lang_20};
const lv_font_t passport_lang_16 = {.get_glyph_dsc = lookup,
                                    .get_glyph_bitmap = bitmap,
                                    .line_height = 19,
                                    .base_line = 4,
                                    .dsc = &supplemental};
const lv_font_t passport_lang_20 = {.get_glyph_dsc = lookup,
                                    .get_glyph_bitmap = bitmap,
                                    .line_height = 22,
                                    .base_line = 4,
                                    .dsc = &supplemental};
