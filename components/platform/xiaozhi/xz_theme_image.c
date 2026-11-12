/* Decode untrusted PNG/GIF only during theme installation. The chat renderer
 * subsequently reads small ready-to-draw frames directly from Flash. */
#include "xz_theme_image.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_rom_crc.h"
#include "miniz.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define CRC(ptr, n) esp_rom_crc32_le(0, (ptr), (n))
#else
#include <zlib.h>
#define CRC(ptr, n) crc32(0, (ptr), (n))
#endif
enum { EDGE = XZ_ICON_EDGE, PIXELS = EDGE * EDGE, MAX_EDGE = 240 };
typedef struct {
  unsigned w, h, dw, dh, left, top, edge;
} fit_t;
static fit_t fit(unsigned w, unsigned h, unsigned edge) {
  unsigned dw = w >= h ? edge : (w * edge + h / 2) / h, dh = h >= w ? edge : (h * edge + w / 2) / w;
  if (!dw)
    dw = 1;
  if (!dh)
    dh = 1;
  return (fit_t){w, h, dw, dh, (edge - dw) / 2, (edge - dh) / 2, edge};
}
static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
unsigned xz_theme_png_size(xz_blob_t b) {
  if (!b.data || b.size < 33 || memcmp(b.data, "\x89PNG\r\n\x1a\n", 8))
    return 0;
  unsigned w = be32(b.data + 16), h = be32(b.data + 20);
  return w && h && w <= 240 && h <= 320 ? (w > h ? w : h) : 0;
}
static int paeth(int a, int b, int c) {
  int p = a + b - c, da = abs(p - a), db = abs(p - b), dc = abs(p - c);
  return da <= db && da <= dc ? a : db <= dc ? b : c;
}
typedef struct {
  unsigned w, h, depth, type, components, interlace;
  const uint8_t *palette, *alpha;
  size_t palette_len, alpha_len, first, packed;
} png_t;
static bool png_chunks(xz_blob_t b, png_t *v) {
  size_t at = 8;
  bool ended = false;
  while (b.size - at >= 12) {
    const uint8_t *tag = b.data + at + 4, *p = tag + 4;
    size_t n = be32(b.data + at);
    if (n > b.size - at - 12 || CRC(tag, n + 4) != be32(p + n))
      return false;
    if (!memcmp(tag, "IHDR", 4)) {
      if (at != 8 || n != 13 || p[10] || p[11] || p[12] > 1)
        return false;
      v->w = be32(p);
      v->h = be32(p + 4);
      v->depth = p[8];
      v->type = p[9];
      v->interlace = p[12];
    } else if (!v->w || !v->h)
      return false;
    else if (!memcmp(tag, "PLTE", 4)) {
      if (!n || n % 3 || n > 768 || v->palette || v->first || v->alpha)
        return false;
      v->palette = p;
      v->palette_len = n / 3;
    } else if (!memcmp(tag, "tRNS", 4)) {
      if (v->alpha || v->first)
        return false;
      v->alpha = p;
      v->alpha_len = n;
    } else if (!memcmp(tag, "IDAT", 4)) {
      if (ended)
        return false;
      if (!v->first)
        v->first = at;
      v->packed += n;
    } else if (!memcmp(tag, "IEND", 4))
      return !n && at + 12 == b.size && v->first && v->packed >= 6;
    else if (!(tag[0] & 32))
      return false;
    if (v->first && memcmp(tag, "IDAT", 4))
      ended = true;
    at += 12 + n;
  }
  return false;
}
static void paint(const fit_t *f, xz_rgba_t *canvas, unsigned x, unsigned y, xz_rgba_t color) {
  unsigned x1 = (x * f->dw + f->w - 1) / f->w, x2 = ((x + 1) * f->dw + f->w - 1) / f->w;
  unsigned y1 = (y * f->dh + f->h - 1) / f->h, y2 = ((y + 1) * f->dh + f->h - 1) / f->h;
  for (unsigned dy = y1; dy < y2; dy++)
    for (unsigned dx = x1; dx < x2; dx++)
      canvas[(dy + f->top) * f->edge + dx + f->left] = color;
}
/* The generator allows a portrait PNG as large as the screen. Keep only two
 * scanlines and the small output icon; never allocate the full source bitmap
 * or join IDAT chunks. Adam7 passes feed the same nearest-neighbour sampler. */
typedef struct {
  png_t png;
  fit_t fit;
  uint8_t rows[2][1 + 240 * 8];
  unsigned pass, x0, y0, dx, dy, w, h, row, y, pos, current;
  bool done;
  xz_rgba_t canvas[];
} png_work_t;
static void png_pass(png_work_t *s) {
  static const uint8_t adam7[7][4] = {
      {0,0,8,8}, {4,0,8,8}, {0,4,4,8}, {2,0,4,4},
      {0,2,2,4}, {1,0,2,2}, {0,1,1,2}};
  for (; s->pass < (s->png.interlace ? 7u : 1u); s->pass++) {
    const uint8_t *a = adam7[s->pass];
    s->x0 = s->png.interlace ? a[0] : 0;
    s->y0 = s->png.interlace ? a[1] : 0;
    s->dx = s->png.interlace ? a[2] : 1;
    s->dy = s->png.interlace ? a[3] : 1;
    s->w = s->png.w > s->x0 ? (s->png.w - s->x0 + s->dx - 1) / s->dx : 0;
    s->h = s->png.h > s->y0 ? (s->png.h - s->y0 + s->dy - 1) / s->dy : 0;
    if (s->w && s->h) {
      s->row = (s->w * s->png.depth * s->png.components + 7) / 8;
      s->y = s->pos = 0;
      return;
    }
  }
  s->done = true;
}
static unsigned png_sample(const uint8_t *p, unsigned index, unsigned depth) {
  if (depth == 16)
    return (unsigned)p[2 * index] << 8 | p[2 * index + 1];
  unsigned bit = index * depth;
  return (p[bit / 8] >> (8 - depth - bit % 8)) & ((1u << depth) - 1);
}
static bool png_row(png_work_t *s) {
  const png_t *v = &s->png;
  uint8_t *p = s->rows[s->current] + 1;
  const uint8_t *previous = s->rows[s->current ^ 1] + 1;
  unsigned filter = p[-1], bytes = (v->depth * v->components + 7) / 8;
  if (filter > 4)
    return false;
  for (unsigned x = 0; x < s->row; x++) {
    unsigned left = x >= bytes ? p[x - bytes] : 0, up = s->y ? previous[x] : 0;
    unsigned ul = s->y && x >= bytes ? previous[x - bytes] : 0;
    if (filter == 1) p[x] += left;
    else if (filter == 2) p[x] += up;
    else if (filter == 3) p[x] += (left + up) / 2;
    else if (filter == 4) p[x] += paeth(left, up, ul);
  }
  for (unsigned x = 0; x < s->w; x++) {
    unsigned q[4] = {0};
    for (unsigned c = 0; c < v->components; c++)
      q[c] = png_sample(p, x * v->components + c, v->depth);
    xz_rgba_t col = {0,0,0,255};
    if (v->type == 3) {
      if (q[0] >= v->palette_len)
        return false;
      col = (xz_rgba_t){v->palette[3*q[0]], v->palette[3*q[0]+1],
                        v->palette[3*q[0]+2], q[0] < v->alpha_len ? v->alpha[q[0]] : 255};
    } else {
      unsigned shift = v->depth == 16 ? 8 : 0;
      if (v->type == 0 || v->type == 4) {
        col.r = col.g = col.b = v->depth < 8 ? q[0] * 255 / ((1u << v->depth) - 1) : q[0] >> shift;
        if (v->type == 4) col.a = q[1] >> shift;
        else if (v->alpha && q[0] == ((unsigned)v->alpha[0] << 8 | v->alpha[1])) col.a = 0;
      } else {
        col.r = q[0] >> shift;
        col.g = q[1] >> shift;
        col.b = q[2] >> shift;
        if (v->type == 6) col.a = q[3] >> shift;
        else if (v->alpha && q[0] == ((unsigned)v->alpha[0] << 8 | v->alpha[1]) &&
                 q[1] == ((unsigned)v->alpha[2] << 8 | v->alpha[3]) &&
                 q[2] == ((unsigned)v->alpha[4] << 8 | v->alpha[5])) col.a = 0;
      }
    }
    paint(&s->fit, s->canvas, s->x0 + x * s->dx, s->y0 + s->y * s->dy, col);
  }
  s->current ^= 1;
  s->pos = 0;
  if (++s->y == s->h) {
    s->pass++;
    png_pass(s);
  }
  return true;
}
static bool png_output(png_work_t *s, const uint8_t *p, size_t n) {
  while (n) {
    if (s->done)
      return false;
    size_t part = s->row + 1 - s->pos;
    if (part > n) part = n;
    memcpy(s->rows[s->current] + s->pos, p, part);
    s->pos += part; p += part; n -= part;
    if (s->pos == s->row + 1 && !png_row(s))
      return false;
  }
  return true;
}
bool xz_theme_decode_png(xz_blob_t b, unsigned edge, xz_frame_writer sink, void *user) {
  if ((edge != 32 && edge != 64) || !xz_theme_png_size(b) || !sink)
    return false;
  png_t v = {0};
  if (!png_chunks(b, &v) || v.w > 240 || v.h > 320)
    return false;
  if (v.type == 3) {
    if (!v.palette || (v.depth != 1 && v.depth != 2 && v.depth != 4 && v.depth != 8) ||
        v.palette_len > (1u << v.depth) || v.alpha_len > v.palette_len)
      return false;
    v.components = 1;
  } else {
    if (v.depth != 8 && v.depth != 16 &&
        !(v.type == 0 && (v.depth == 1 || v.depth == 2 || v.depth == 4)))
      return false;
    switch (v.type) {
    case 0: v.components = 1; break;
    case 2: v.components = 3; break;
    case 4: v.components = 2; break;
    case 6: v.components = 4; break;
    default: return false;
    }
    if (v.alpha && !((v.type == 0 && v.alpha_len == 2) || (v.type == 2 && v.alpha_len == 6)))
      return false;
  }
  enum { WINDOW = 32768 };
  png_work_t *s = calloc(1, sizeof *s + edge * edge * sizeof(xz_rgba_t));
  uint8_t *window = malloc(WINDOW);
#ifdef ESP_PLATFORM
  tinfl_decompressor *z = malloc(sizeof *z);
#else
  z_stream *z = calloc(1, sizeof *z);
  bool initialized = false;
#endif
  bool ok = false;
  if (!s || !window || !z)
    goto end;
  s->png = v; s->fit = fit(v.w,v.h,edge); png_pass(s);
#ifdef ESP_PLATFORM
  tinfl_init(z);
#else
  if (inflateInit(z) != Z_OK) goto end;
  initialized = true;
#endif
  size_t at = v.first, pos = 0, consumed = 0, out_at = 0;
  for (;;) {
    size_t chunk = be32(b.data + at);
    while (pos == chunk && consumed < v.packed) {
      at += 12 + chunk; pos = 0; chunk = be32(b.data + at);
    }
    size_t input = chunk - pos, output = WINDOW - out_at;
#ifdef ESP_PLATFORM
    unsigned flags = TINFL_FLAG_PARSE_ZLIB_HEADER;
    if (input < v.packed - consumed) flags |= TINFL_FLAG_HAS_MORE_INPUT;
    tinfl_status result = tinfl_decompress(z, b.data + at + 8 + pos, &input,
                                         window, window + out_at, &output, flags);
    bool failed = result < TINFL_STATUS_DONE, done = result == TINFL_STATUS_DONE;
#else
    z->next_in = (uint8_t *)b.data + at + 8 + pos; z->avail_in = input;
    z->next_out = window + out_at; z->avail_out = output;
    int result = inflate(z,Z_NO_FLUSH);
    input -= z->avail_in; output -= z->avail_out;
    bool failed = result != Z_OK && result != Z_STREAM_END, done = result == Z_STREAM_END;
#endif
    pos += input; consumed += input;
    if (failed || !png_output(s,window + out_at,output))
      break;
    out_at = (out_at + output) % WINDOW;
    if (done) {
      ok = consumed == v.packed && s->done && sink(user,s->canvas,0);
      break;
    }
    if (!input && !output) break;
#ifdef ESP_PLATFORM
    if (output) vTaskDelay(1);
#endif
  }
end:
#ifndef ESP_PLATFORM
  if (initialized) inflateEnd(z);
#endif
  free(z); free(window); free(s);
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
  fit_t f = fit(w, h, EDGE);
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
    return xz_theme_decode_png(b, EDGE, sink, user);
  if (!memcmp(b.data, "GIF89a", 6) || !memcmp(b.data, "GIF87a", 6))
    return decode_gif(b, limit, sink, user);
  return false;
}
