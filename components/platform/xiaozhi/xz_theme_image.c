/* Decode untrusted PNG/GIF only during theme installation. The chat renderer
 * subsequently reads small ready-to-draw frames directly from Flash. */
#include "xz_theme_image.h"
#include "puff.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#define CRC(ptr, n) esp_rom_crc32_le(0, (ptr), (n))
#else
#include <zlib.h>
#define CRC(ptr, n) crc32(0, (ptr), (n))
#endif
enum { EDGE = XZ_ICON_EDGE, PIXELS = EDGE * EDGE, MAX_EDGE = 240 };
typedef struct {
  unsigned w, h, dw, dh, left, top;
} fit_t;
static fit_t fit(unsigned w, unsigned h) {
  unsigned dw = w >= h ? EDGE : (w * EDGE + h / 2) / h, dh = h >= w ? EDGE : (h * EDGE + w / 2) / w;
  if (!dw)
    dw = 1;
  if (!dh)
    dh = 1;
  return (fit_t){w, h, dw, dh, (EDGE - dw) / 2, (EDGE - dh) / 2};
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int paeth(int a, int b, int c) {
  int p = a + b - c, da = abs(p - a), db = abs(p - b), dc = abs(p - c);
  return da <= db && da <= dc ? a : db <= dc ? b : c;
}
typedef struct {
  unsigned w, h, depth, type, components;
  const uint8_t *palette, *alpha, *first;
  size_t palette_len, alpha_len, packed, parts;
} png_t;
static bool png_chunks(xz_blob_t b, png_t *v, uint8_t *copy) {
  size_t at = 8, filled = 0;
  while (b.size - at >= 12) {
    const uint8_t *tag = b.data + at + 4, *p = tag + 4;
    size_t n = be32(b.data + at);
    if (n > b.size - at - 12 || CRC(tag, n + 4) != be32(p + n))
      return false;
    if (!memcmp(tag, "IHDR", 4)) {
      if (at != 8 || n != 13 || p[10] || p[11] || p[12])
        return false;
      v->w = be32(p);
      v->h = be32(p + 4);
      v->depth = p[8];
      v->type = p[9];
    } else if (!memcmp(tag, "PLTE", 4)) {
      if (!n || n % 3 || n > 768)
        return false;
      v->palette = p;
      v->palette_len = n / 3;
    } else if (!memcmp(tag, "tRNS", 4)) {
      v->alpha = p;
      v->alpha_len = n;
    } else if (!memcmp(tag, "IDAT", 4)) {
      if (copy) {
        memcpy(copy + filled, p, n);
        filled += n;
      } else {
        if (!v->first)
          v->first = p;
        v->packed += n;
        v->parts++;
      }
    } else if (!memcmp(tag, "IEND", 4))
      return !n && at + 12 == b.size && v->first;
    else if (!(tag[0] & 32))
      return false;
    at += 12 + n;
  }
  return false;
}
static bool decode_png(xz_blob_t b, xz_frame_writer sink, void *user) {
  png_t v = {0};
  if (!png_chunks(b, &v, NULL) || !v.w || !v.h || v.w > MAX_EDGE || v.h > MAX_EDGE || v.packed < 6)
    return false;
  if (v.type == 3) {
    if (!v.palette || v.alpha_len > v.palette_len ||
        (v.depth != 1 && v.depth != 2 && v.depth != 4 && v.depth != 8))
      return false;
    v.components = 1;
  } else {
    if (v.depth != 8)
      return false;
    switch (v.type) {
    case 0:
      v.components = 1;
      break;
    case 2:
      v.components = 3;
      break;
    case 4:
      v.components = 2;
      break;
    case 6:
      v.components = 4;
      break;
    default:
      return false;
    }
  }
  size_t row = (v.w * v.depth * v.components + 7) / 8, total = (row + 1) * v.h;
  if (total > 70000 || v.packed > 80000)
    return false;
  uint8_t *joined = v.parts > 1 ? malloc(v.packed) : NULL, *raw = malloc(total);
  xz_rgba_t *canvas = calloc(PIXELS, sizeof *canvas);
  bool ok = false;
  if (!raw || !canvas || (v.parts > 1 && (!joined || !png_chunks(b, &v, joined))))
    goto end;
  const uint8_t *src = joined ? joined : v.first;
  if ((src[0] & 15) != 8 || (src[0] >> 4) > 7 || ((unsigned)src[0] * 256 + src[1]) % 31 ||
      (src[1] & 32))
    goto end;
  unsigned long in = v.packed - 6, out = total;
  if (puff(raw, &out, src + 2, &in) || out != total || in != v.packed - 6)
    goto end;
  unsigned a = 1, c = 0;
  for (size_t i = 0; i < total; i++) {
    a = (a + raw[i]) % 65521;
    c = (c + a) % 65521;
  }
  if (((uint32_t)c << 16 | a) != be32(src + v.packed - 4))
    goto end;
  unsigned bytes = (v.depth * v.components + 7) / 8;
  for (unsigned y = 0; y < v.h; y++) {
    uint8_t *p = raw + y * (row + 1) + 1;
    unsigned filter = p[-1];
    if (filter > 4)
      goto end;
    for (unsigned x = 0; x < row; x++) {
      unsigned left = x >= bytes ? p[x - bytes] : 0,
               up = y ? p[(ptrdiff_t)x - (ptrdiff_t)row - 1] : 0;
      unsigned ul = y && x >= bytes ? p[(ptrdiff_t)x - bytes - (ptrdiff_t)row - 1] : 0;
      if (filter == 1)
        p[x] += left;
      else if (filter == 2)
        p[x] += up;
      else if (filter == 3)
        p[x] += (left + up) / 2;
      else if (filter == 4)
        p[x] += paeth(left, up, ul);
    }
  }
  fit_t f = fit(v.w, v.h);
  for (unsigned y = 0; y < f.dh; y++)
    for (unsigned x = 0; x < f.dw; x++) {
      unsigned sx = x * v.w / f.dw, sy = y * v.h / f.dh;
      const uint8_t *p = raw + sy * (row + 1) + 1;
      xz_rgba_t col = {0, 0, 0, 255};
      if (v.type == 3) {
        unsigned bit = sx * v.depth,
                 index = (p[bit / 8] >> (8 - v.depth - (bit % 8))) & ((1u << v.depth) - 1);
        if (index >= v.palette_len)
          goto end;
        col = (xz_rgba_t){v.palette[3 * index], v.palette[3 * index + 1], v.palette[3 * index + 2],
                          index < v.alpha_len ? v.alpha[index] : 255};
      } else {
        p += sx * v.components;
        if (v.type == 0 || v.type == 4) {
          col.r = col.g = col.b = p[0];
          if (v.type == 4)
            col.a = p[1];
          else if (v.alpha_len == 2 && !v.alpha[0] && v.alpha[1] == p[0])
            col.a = 0;
        } else {
          col.r = p[0];
          col.g = p[1];
          col.b = p[2];
          if (v.type == 6)
            col.a = p[3];
          else if (v.alpha_len == 6 && !v.alpha[0] && !v.alpha[2] && !v.alpha[4] &&
                   v.alpha[1] == p[0] && v.alpha[3] == p[1] && v.alpha[5] == p[2])
            col.a = 0;
        }
      }
      canvas[(f.top + y) * EDGE + f.left + x] = col;
    }
  ok = sink(user, canvas, 0);
end:
  free(joined);
  free(raw);
  free(canvas);
  return ok;
}
typedef struct {
  const uint8_t *p, *end;
  unsigned left, bitbuf, bits;
} gif_stream_t;
static bool gif_byte(gif_stream_t *s, unsigned *out) {
  if (!s->left) {
    if (s->p >= s->end || !(s->left = *s->p++) || (size_t)(s->end - s->p) < s->left)
      return false;
  }
  *out = *s->p++;
  s->left--;
  return true;
}
static bool gif_code(gif_stream_t *s, unsigned n, unsigned *value) {
  while (s->bits < n) {
    unsigned byte;
    if (!gif_byte(s, &byte))
      return false;
    s->bitbuf |= byte << s->bits;
    s->bits += 8;
  }
  *value = s->bitbuf & ((1u << n) - 1);
  s->bitbuf >>= n;
  s->bits -= n;
  return true;
}
static bool skip_blocks(const uint8_t **ptr, const uint8_t *end) {
  const uint8_t *p = *ptr;
  while (p < end) {
    size_t n = *p++;
    if (!n) {
      *ptr = p;
      return true;
    }
    if ((size_t)(end - p) < n)
      return false;
    p += n;
  }
  return false;
}
typedef struct {
  uint16_t prefix[4096];
  uint8_t suffix[4096], stack[4097];
  xz_rgba_t canvas[PIXELS], previous[PIXELS];
} gif_work_t;
typedef struct {
  unsigned x, y, w, h;
  const uint8_t *palette;
  unsigned colors, transparent;
  bool interlace;
} gif_frame_t;
static void paint(const fit_t *f, xz_rgba_t *canvas, unsigned x, unsigned y, xz_rgba_t color) {
  unsigned x1 = (x * f->dw + f->w - 1) / f->w, x2 = ((x + 1) * f->dw + f->w - 1) / f->w;
  unsigned y1 = (y * f->dh + f->h - 1) / f->h, y2 = ((y + 1) * f->dh + f->h - 1) / f->h;
  for (unsigned dy = y1; dy < y2; dy++)
    for (unsigned dx = x1; dx < x2; dx++)
      canvas[(dy + f->top) * EDGE + dx + f->left] = color;
}
static bool gif_pixels(gif_stream_t *s, gif_work_t *work, const fit_t *fit, const gif_frame_t *f,
                       unsigned minbits) {
  if (minbits < 2 || minbits > 8)
    return false;
  unsigned clear = 1u << minbits, width = minbits + 1, next = clear + 2, old = 4096, first = 0,
           output = 0, x = 0, y = 0, pass = 0;
  unsigned code, steps = 0;
  while (gif_code(s, width, &code)) {
    if (++steps > f->w * f->h * 3 + 4096)
      return false;
    if (code == clear) {
      width = minbits + 1;
      next = clear + 2;
      old = 4096;
      continue;
    }
    if (code == clear + 1)
      return output == f->w * f->h;
    if (code > next || code >= 4096 || (old == 4096 && code >= clear))
      return false;
    unsigned cur = code, sp = 0;
    if (code == next) {
      work->stack[sp++] = first;
      cur = old;
    }
    while (cur >= clear) {
      if (cur < clear + 2 || cur >= next || sp >= 4096)
        return false;
      work->stack[sp++] = work->suffix[cur];
      cur = work->prefix[cur];
    }
    first = cur;
    work->stack[sp++] = cur;
    if (old != 4096 && next < 4096) {
      work->prefix[next] = old;
      work->suffix[next++] = first;
      if (next == (1u << width) && width < 12)
        width++;
    }
    old = code;
    while (sp) {
      unsigned idx = work->stack[--sp];
      if (output++ >= f->w * f->h || idx >= f->colors || y >= f->h)
        return false;
      if (idx != f->transparent) {
        const uint8_t *p = f->palette + 3 * idx;
        paint(fit, work->canvas, f->x + x, f->y + y, (xz_rgba_t){p[0], p[1], p[2], 255});
      }
      if (++x == f->w) {
        x = 0;
        if (!f->interlace)
          y++;
        else {
          static const unsigned jump[] = {8, 8, 4, 2}, start[] = {0, 4, 2, 1};
          y += jump[pass];
          while (y >= f->h && pass < 3)
            y = start[++pass];
        }
      }
    }
  }
  return false;
}
static bool decode_gif(xz_blob_t b, unsigned limit, xz_frame_writer sink, void *user) {
  if (b.size < 14)
    return false;
  const uint8_t *p = b.data + 13, *end = b.data + b.size;
  unsigned w = xz_u16(b.data + 6), h = xz_u16(b.data + 8);
  if (!w || !h || w > MAX_EDGE || h > MAX_EDGE)
    return false;
  unsigned palette_n = (b.data[10] & 128) ? 2u << (b.data[10] & 7) : 0;
  if ((size_t)(end - p) < 3 * palette_n)
    return false;
  const uint8_t *palette = p;
  p += 3 * palette_n;
  gif_work_t *work = calloc(1, sizeof *work);
  if (!work)
    return false;
  fit_t f = fit(w, h);
  unsigned frames = 0, trans = 256, delay = 100, disposal = 0, walked = 0;
  bool ok = false;
  while (p < end && walked++ < 512) {
    unsigned block = *p++;
    if (block == 0x3b) {
      ok = frames > 0;
      break;
    }
    if (block == 0x21) {
      if (p >= end)
        break;
      if (*p++ == 0xf9) {
        if (end - p < 6 || p[0] != 4 || p[5] != 0)
          break;
        disposal = (p[1] >> 2) & 7;
        trans = (p[1] & 1) ? p[4] : 256;
        delay = xz_u16(p + 2) * 10;
        if (delay < 80)
          delay = 80;
        if (delay > 5000)
          delay = 5000;
        p += 6;
      } else if (!skip_blocks(&p, end))
        break;
      continue;
    }
    if (block != 0x2c || end - p < 10)
      break;
    gif_frame_t img = {.x = xz_u16(p),
                       .y = xz_u16(p + 2),
                       .w = xz_u16(p + 4),
                       .h = xz_u16(p + 6),
                       .palette = palette,
                       .colors = palette_n,
                       .transparent = trans,
                       .interlace = (p[8] & 64) != 0};
    unsigned flags = p[8];
    p += 9;
    if (!img.w || !img.h || img.x + img.w > w || img.y + img.h > h)
      break;
    if (flags & 128) {
      img.colors = 2u << (flags & 7);
      if ((size_t)(end - p) < img.colors * 3)
        break;
      img.palette = p;
      p += img.colors * 3;
    }
    if (!img.colors || p >= end)
      break;
    if (disposal == 3)
      memcpy(work->previous, work->canvas, sizeof work->canvas);
    unsigned minbits = *p++;
    gif_stream_t s = {.p = p, .end = end};
    if (!gif_pixels(&s, work, &f, &img, minbits) || !skip_blocks(&p, end) ||
        !sink(user, work->canvas, delay))
      break;
    if (++frames == limit) {
      ok = true;
      break;
    }
    if (disposal == 3)
      memcpy(work->canvas, work->previous, sizeof work->canvas);
    if (disposal == 2)
      for (unsigned y = 0; y < img.h; y++)
        for (unsigned x = 0; x < img.w; x++)
          paint(&f, work->canvas, img.x + x, img.y + y, (xz_rgba_t){0});
    trans = 256;
    delay = 100;
    disposal = 0;
  }
  free(work);
  return ok;
}
bool xz_theme_decode_image(xz_blob_t b, unsigned limit, xz_frame_writer sink, void *user) {
  if (!b.data || b.size < 8 || !limit || !sink)
    return false;
  if (!memcmp(b.data, "\x89PNG\r\n\x1a\n", 8))
    return decode_png(b, sink, user);
  if (!memcmp(b.data, "GIF89a", 6) || !memcmp(b.data, "GIF87a", 6))
    return decode_gif(b, limit, sink, user);
  return false;
}
