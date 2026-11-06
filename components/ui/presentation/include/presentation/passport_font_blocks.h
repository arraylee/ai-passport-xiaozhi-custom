#pragma once
#include "lvgl.h"

#define PASSPORT_FONT_BLOCK_BYTES 512u

typedef struct {
    /* First member: LVGL's ordinary CMap/metrics lookup remains unchanged. */
    lv_font_fmt_txt_dsc_t fmt;
    const uint8_t *compressed;
    const uint32_t *block_offsets; /* block_count + 1 entries */
    uint32_t raw_size;
    uint32_t part_base;
    uint32_t block_count;
    uint32_t compressed_size;
    uint32_t glyph_count; /* Includes reserved glyph zero. */
} passport_font_block_dsc_t;

/* Converts verified native packed 4bpp to the caller's A8 draw buffer.
 * No static raw-bitmap pointer is exposed; generated fonts set static_bitmap=0.
 * Uses one temporary 512-byte heap block and bounded stack-local DEFLATE state.
 * Host and ESP32-C3 use the same header/checksum/exact-length validation.
 * Temporary memory is released before return, including failure. */
const void *passport_font_get_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *draw_buf);
