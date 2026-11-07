#pragma once
#include "xz_theme.h"
typedef struct {
  xz_blob_t bitmap, metrics, maps;
  unsigned map_count, bpp, format, height, baseline;
} xz_theme_font_t;
typedef struct {
  unsigned offset, end, advance, width, height;
  int x, y;
} xz_theme_glyph_t;
bool xz_theme_font_open(xz_blob_t file, xz_theme_font_t *font);
bool xz_theme_glyph(const xz_theme_font_t *font, unsigned cp, xz_theme_glyph_t *out);
bool xz_theme_pixels(const xz_theme_font_t *font, const xz_theme_glyph_t *g, uint8_t *dst,
                     unsigned stride);
