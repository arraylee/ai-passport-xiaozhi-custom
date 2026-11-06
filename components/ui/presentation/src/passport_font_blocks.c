#include "presentation/passport_font_blocks.h"
#include <stdlib.h>
#include <string.h>
#include "../vendor/puff/puff.h"

typedef struct {
    uint8_t bytes[PASSPORT_FONT_BLOCK_BYTES];
} block_workspace_t;
_Static_assert(sizeof(block_workspace_t) == PASSPORT_FONT_BLOCK_BYTES,
               "Rare font decoder must use only one temporary block");

static bool decode_block(const passport_font_block_dsc_t *font, uint32_t block,
                         block_workspace_t *work) {
    if (block >= font->block_count) return false;
    uint32_t start = font->block_offsets[block], end = font->block_offsets[block + 1];
    if (end <= start || end > font->compressed_size || end - start < 6u) return false;
    size_t expected = font->raw_size - block * PASSPORT_FONT_BLOCK_BYTES;
    if (expected > PASSPORT_FONT_BLOCK_BYTES) expected = PASSPORT_FONT_BLOCK_BYTES;
    const uint8_t *source = font->compressed + start;
    size_t size = end - start;
    /* RFC1950: DEFLATE, <=32KiB window, valid FCHECK, no preset dictionary. */
    unsigned header = (unsigned)source[0] * 256u + source[1];
    if ((source[0] & 15u) != 8u || (source[0] >> 4) > 7u ||
        header % 31u || (source[1] & 32u)) return false;
    unsigned long input_size = (unsigned long)(size - 6u), output_size = (unsigned long)expected;
    if (puff(work->bytes, &output_size, source + 2, &input_size) != 0 ||
        input_size != size - 6u || output_size != expected) return false;
    /* Check Adler32 without another allocation or a target-specific decoder. */
    uint32_t low = 1, high = 0;
    for (size_t i = 0; i < expected; ++i) {
        low += work->bytes[i];
        if (low >= 65521u) low -= 65521u;
        high += low;
        if (high >= 65521u) high -= 65521u;
    }
    const uint8_t *tail = source + size - 4;
    uint32_t checksum = (uint32_t)tail[0] << 24 | (uint32_t)tail[1] << 16 |
                        (uint32_t)tail[2] << 8 | tail[3];
    return checksum == (high << 16 | low);
}

const void *passport_font_get_bitmap(lv_font_glyph_dsc_t *glyph, lv_draw_buf_t *draw_buf) {
    if (!glyph || !glyph->resolved_font || !glyph->resolved_font->dsc ||
        glyph->req_raw_bitmap || !draw_buf || !draw_buf->data) return NULL;
    const passport_font_block_dsc_t *font = glyph->resolved_font->dsc;
    uint32_t count = font->raw_size / PASSPORT_FONT_BLOCK_BYTES +
                     (font->raw_size % PASSPORT_FONT_BLOCK_BYTES != 0);
    if (!font->compressed || !font->block_offsets || !font->fmt.glyph_dsc ||
        font->fmt.bpp != 4 || font->fmt.stride != 0 || font->block_count != count ||
        !glyph->gid.index || glyph->gid.index >= font->glyph_count) return NULL;
    const lv_font_fmt_txt_glyph_dsc_t *metrics = &font->fmt.glyph_dsc[glyph->gid.index];
    if (!metrics->box_w || !metrics->box_h || glyph->box_w != metrics->box_w ||
        glyph->box_h != metrics->box_h || draw_buf->header.cf != LV_COLOR_FORMAT_A8 ||
        draw_buf->header.w < metrics->box_w || draw_buf->header.h < metrics->box_h) return NULL;
    uint32_t pixels = (uint32_t)metrics->box_w * metrics->box_h;
    uint32_t bytes = (pixels + 1u) / 2u;
    if (font->part_base > font->raw_size || metrics->bitmap_index > font->raw_size - font->part_base)
        return NULL;
    uint32_t begin = font->part_base + metrics->bitmap_index;
    if (bytes > font->raw_size - begin) return NULL;
    uint32_t stride = draw_buf->header.stride;
    if (stride < metrics->box_w || metrics->box_h > draw_buf->data_size / stride) return NULL;

    /* Rare glyphs also occur in live replies: keep their temporary output
     * within512B so rendering cannot consume a full Wi-Fi RX buffer's headroom.
     * Bounded DEFLATE state lives on the caller stack, with no static cache. */
    block_workspace_t *work = malloc(sizeof(*work));
    if (!work) return NULL;
    uint32_t active_block = UINT32_MAX;
    for (uint32_t i = 0; i < pixels; ++i) {
        uint32_t position = begin + i / 2u;
        uint32_t block = position / PASSPORT_FONT_BLOCK_BYTES;
        if (block != active_block) {
            if (!decode_block(font, block, work)) { free(work); return NULL; }
            active_block = block;
        }
        uint8_t packed = work->bytes[position % PASSPORT_FONT_BLOCK_BYTES];
        uint8_t alpha = (i & 1u) ? (packed & 15u) : (packed >> 4);
        draw_buf->data[(i / metrics->box_w) * stride + i % metrics->box_w] = alpha * 17u;
    }
    free(work);
    lv_draw_buf_flush_cache(draw_buf, NULL);
    return draw_buf;
}
