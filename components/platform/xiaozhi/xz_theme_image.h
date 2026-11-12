#pragma once
#include "xz_theme.h"
typedef struct {
  uint8_t r, g, b, a;
} xz_rgba_t;
typedef bool (*xz_frame_writer)(void *user, const xz_rgba_t *canvas, unsigned delay_ms);
bool xz_theme_decode_image(xz_blob_t data, unsigned frame_limit, xz_frame_writer sink, void *user);
unsigned xz_theme_png_size(xz_blob_t data);
bool xz_theme_decode_png(xz_blob_t data, unsigned edge, xz_frame_writer sink, void *user);
