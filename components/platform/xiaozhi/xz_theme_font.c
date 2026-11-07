/* 32-bit XiaoZhi CBIN uses relative offsets and 16-byte glyph metrics.
 * Read every number as bytes: an assets.bin entry need not be word aligned. */
#include "xz_theme_font.h"
#include <string.h>

bool xz_theme_font_open(xz_blob_t b, xz_theme_font_t *f) {
  memset(f, 0, sizeof *f);
  if (b.size < 60)
    return false;
  const uint8_t *p = b.data, *d = p + 36;
  unsigned h = xz_u32(p + 12), baseline = xz_u32(p + 16), a = xz_u32(d), m = xz_u32(d + 4),
           c = xz_u32(d + 8);
  unsigned flags = xz_u16(d + 18), maps = flags & 511, bpp = (flags >> 9) & 15,
           format = flags >> 14;
  if (xz_u32(p + 24) != 36 || h < 4 || h > 96 || baseline > h || a < 24 || a > m || m >= c ||
      c > b.size - 36 || (c - m) % 16 || !maps || maps > 128 ||
      (bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8) || format > 2 || maps * 20 > b.size - 36 - c)
    return false;
  f->bitmap = (xz_blob_t){d + a, m - a};
  f->metrics = (xz_blob_t){d + m, c - m};
  // The online generator appends a zero metric after the last real glyph.
  static const uint8_t padding[16] = {0};
  if (f->metrics.size >= 32 && !memcmp(f->metrics.data + f->metrics.size - 16, padding, 16))
    f->metrics.size -= 16;
  f->maps = (xz_blob_t){d + c, b.size - 36 - c};
  f->height = h;
  f->baseline = baseline;
  f->map_count = maps;
  f->bpp = bpp;
  f->format = format;
  unsigned prev = 0;
  for (size_t i = 0; i < f->metrics.size; i += 16) {
    const uint8_t *g = f->metrics.data + i;
    unsigned offset = xz_u32(g), w = xz_u16(g + 8), gh = xz_u16(g + 10);
    if (offset < prev || offset > f->bitmap.size || w > 128 || gh > 128 || xz_u32(g + 4) > 2048)
      return false;
    prev = offset;
    size_t end = i + 16 < f->metrics.size ? xz_u32(g + 16) : f->bitmap.size;
    if (end < offset || end > f->bitmap.size || (!format && (w * gh * bpp + 7) / 8 > end - offset))
      return false;
  }
  for (unsigned i = 0; i < maps; i++) {
    const uint8_t *v = f->maps.data + 20 * i;
    unsigned n = xz_u16(v + 16), u = xz_u32(v + 8), ids = xz_u32(v + 12), type = v[18];
    if (type > 3 || !xz_u16(v + 4) || xz_u16(v + 6) >= f->metrics.size / 16)
      return false;
    if (type == 1 || type == 3) {
      if (!n || u < maps * 20 || u > f->maps.size || 2 * n > f->maps.size - u)
        return false;
    }
    if (type <= 1) {
      size_t len = type == 0 ? xz_u16(v + 4) : 2 * n;
      if (ids < maps * 20 || ids > f->maps.size || len > f->maps.size - ids)
        return false;
    }
  }
  return true;
}
bool xz_theme_glyph(const xz_theme_font_t *f, unsigned cp, xz_theme_glyph_t *out) {
  for (unsigned i = 0; i < f->map_count; i++) {
    const uint8_t *m = f->maps.data + 20 * i;
    unsigned rel = cp - xz_u32(m), width = xz_u16(m + 4), gid = xz_u16(m + 6), type = m[18];
    if (rel >= width)
      continue;
    if (type == 2)
      gid += rel;
    else if (type == 0)
      gid += f->maps.data[xz_u32(m + 12) + rel];
    else {
      const uint8_t *codes = f->maps.data + xz_u32(m + 8);
      unsigned count = xz_u16(m + 16), lo = 0, hi = count;
      while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (xz_u16(codes + 2 * mid) < rel)
          lo = mid + 1;
        else
          hi = mid;
      }
      if (lo == count || xz_u16(codes + 2 * lo) != rel)
        continue;
      gid += type == 3 ? lo : xz_u16(f->maps.data + xz_u32(m + 12) + lo * 2);
    }
    if (!gid || gid >= f->metrics.size / 16)
      return false;
    const uint8_t *g = f->metrics.data + gid * 16;
    *out =
        (xz_theme_glyph_t){.offset = xz_u32(g),
                           .end = gid + 1 < f->metrics.size / 16 ? xz_u32(g + 16) : f->bitmap.size,
                           .advance = (xz_u32(g + 4) + 8) / 16,
                           .width = xz_u16(g + 8),
                           .height = xz_u16(g + 10),
                           .x = (int16_t)xz_u16(g + 12),
                           .y = (int16_t)xz_u16(g + 14)};
    return true;
  }
  return false;
}
typedef struct {
  const uint8_t *source;
  unsigned pos, limit;
  bool failed;
} bits_t;
static unsigned read_bits(bits_t *s, unsigned n) {
  if (n > s->limit - s->pos) {
    s->failed = true;
    return 0;
  }
  unsigned v = 0;
  while (n--) {
    v = v * 2 + ((s->source[s->pos / 8] >> (7 - (s->pos % 8))) & 1);
    s->pos++;
  }
  return v;
}
bool xz_theme_pixels(const xz_theme_font_t *f, const xz_theme_glyph_t *g, uint8_t *dst,
                     unsigned stride) {
  if (!dst || stride < g->width || g->end > f->bitmap.size || g->offset > g->end ||
      g->width > 128 || g->height > 128)
    return false;
  bits_t s = {.source = f->bitmap.data + g->offset, .limit = (g->end - g->offset) * 8};
  unsigned state = 0, last = 0, count = 0, max = (1u << f->bpp) - 1;
  uint8_t previous_line[128] = {0};
  for (unsigned y = 0; y < g->height; y++)
    for (unsigned x = 0; x < g->width; x++) {
      unsigned value = last;
      if (!f->format)
        value = read_bits(&s, f->bpp);
      else if (!state) {
        bool has_previous = s.pos != 0;
        value = read_bits(&s, f->bpp);
        if (has_previous && value == last) {
          state = 1;
          count = 0;
        }
      } else if (state == 1) {
        count++;
        if (!read_bits(&s, 1)) {
          value = read_bits(&s, f->bpp);
          state = 0;
        } else if (count == 11) {
          count = read_bits(&s, 6);
          if (count)
            state = 2;
          else {
            value = read_bits(&s, f->bpp);
            state = 0;
          }
        }
      } else if (!--count) {
        value = read_bits(&s, f->bpp);
        state = 0;
      }
      if (s.failed)
        return false;
      last = value;
      if (f->format == 1) {
        value ^= previous_line[x];
        previous_line[x] = value;
      }
      dst[y * stride + x] = (value * 255 + max / 2) / max;
    }
  return true;
}
