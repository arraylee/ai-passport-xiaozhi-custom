#include "presentation/passport_font_blocks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "font_heap_cap.h"

static uint8_t raw[PASSPORT_FONT_BLOCK_BYTES + 204], packed[2 * PASSPORT_FONT_BLOCK_BYTES + 512];
static uint32_t offsets[3];
static lv_font_fmt_txt_glyph_dsc_t metrics[2] = {
    {0}, {.bitmap_index = PASSPORT_FONT_BLOCK_BYTES - 6, .adv_w = 320, .box_w = 20, .box_h = 20}
};
static passport_font_block_dsc_t descriptor = {
    .fmt = {.glyph_dsc = metrics, .bpp = 4, .bitmap_format = LV_FONT_FMT_TXT_PLAIN},
    .compressed = packed, .block_offsets = offsets,
    .raw_size = sizeof(raw), .block_count = 2, .glyph_count = 2
};
static lv_font_t font = {.dsc = &descriptor, .get_glyph_bitmap = passport_font_get_bitmap};

static void compress_blocks(int level, int strategy) {
    offsets[0]=0;
    for (unsigned block=0;block<2;++block) {
        unsigned start=block*PASSPORT_FONT_BLOCK_BYTES;
        unsigned length=sizeof raw-start;
        if(length>PASSPORT_FONT_BLOCK_BYTES)length=PASSPORT_FONT_BLOCK_BYTES;
        z_stream stream={0};
        assert(deflateInit2(&stream,level,Z_DEFLATED,15,8,strategy)==Z_OK);
        stream.next_in=raw+start;stream.avail_in=length;
        stream.next_out=packed+offsets[block];stream.avail_out=sizeof packed-offsets[block];
        assert(deflate(&stream,Z_FINISH)==Z_STREAM_END);
        offsets[block+1]=offsets[block]+stream.total_out;
        assert(deflateEnd(&stream)==Z_OK);
    }
    descriptor.compressed_size=offsets[2];
}

int main(void) {
    lv_init();
    for (unsigned i = 0; i < sizeof(raw); ++i) raw[i] = (uint8_t)(i * 37u + 13u);
    compress_blocks(9,Z_DEFAULT_STRATEGY);
    lv_font_glyph_dsc_t glyph = {.resolved_font = &font, .gid.index = 1, .box_w = 20, .box_h = 20};
    lv_draw_buf_t *out = lv_draw_buf_create(20, 20, LV_COLOR_FORMAT_A8, 0); assert(out);
    uint32_t stride = lv_draw_buf_width_to_stride(20, LV_COLOR_FORMAT_A8);
    /* The glyph crosses a block boundary into the short last block. */
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        assert(passport_font_get_bitmap(&glyph, out) == out);
        for (unsigned i = 0; i < 400; ++i) {
            uint8_t value = raw[PASSPORT_FONT_BLOCK_BYTES - 6 + i / 2];
            value = (i & 1) ? (value & 15) : (value >> 4);
            assert(out->data[(i / 20) * stride + i % 20] == value * 17);
        }
    }
    /* Explicit stored and fixed-Huffman streams share the production decoder. */
    for(unsigned mode=0;mode<2;++mode) {
        compress_blocks(mode ? 9 : 0, mode ? Z_FIXED : Z_DEFAULT_STRATEGY);
        assert(((packed[2]>>1)&3u)==mode);
        assert(passport_font_get_bitmap(&glyph,out)==out && font_heap_live==0);
        for(unsigned i=0;i<400;++i) {
            uint8_t value=raw[PASSPORT_FONT_BLOCK_BYTES - 6 + i/2];value=i&1 ? value&15 : value>>4;
            assert(out->data[(i/20)*stride+i%20]==value*17);
        }
    }
    compress_blocks(9,Z_DEFAULT_STRATEGY);
    /* All four-bit levels survive; parts use local offsets without 20-bit overflow. */
    descriptor.part_base = PASSPORT_FONT_BLOCK_BYTES - 96; metrics[1].bitmap_index = 90;
    assert(passport_font_get_bitmap(&glyph, out) == out);
    descriptor.part_base = 0; metrics[1].bitmap_index = PASSPORT_FONT_BLOCK_BYTES - 6;
    font_heap_limit = 511;
    assert(!passport_font_get_bitmap(&glyph, out) && font_heap_live == 0);
    font_heap_limit = 512;
    glyph.req_raw_bitmap = 1; assert(!passport_font_get_bitmap(&glyph, out)); glyph.req_raw_bitmap = 0;
    glyph.gid.index = 2; assert(!passport_font_get_bitmap(&glyph, out)); glyph.gid.index = 1;
    metrics[1].bitmap_index = sizeof raw - 1; assert(!passport_font_get_bitmap(&glyph, out)); metrics[1].bitmap_index = PASSPORT_FONT_BLOCK_BYTES - 6;
    descriptor.compressed_size--; assert(!passport_font_get_bitmap(&glyph, out)); descriptor.compressed_size++;
    uint8_t saved = packed[offsets[1]]; packed[offsets[1]] ^= 0xff;
    assert(!passport_font_get_bitmap(&glyph, out)); packed[offsets[1]] = saved;
    descriptor.raw_size--; assert(!passport_font_get_bitmap(&glyph, out)); descriptor.raw_size++;
    /* Valid RFC1950 header syntax must not bypass the actual stream/checksum. */
    uint8_t original[sizeof packed]; memcpy(original, packed, sizeof packed);
    for (unsigned damaged = 0; damaged < 7; ++damaged) {
        memcpy(packed, original, sizeof packed);
        if (damaged < 3) {
            unsigned cmf = damaged == 0 ? 0x79u : damaged == 1 ? 0x88u : 0x78u;
            unsigned flg = damaged == 2 ? 0x20u : 0;
            flg += (31u - ((cmf * 256u + flg) % 31u)) % 31u;
            packed[0] = (uint8_t)cmf; packed[1] = (uint8_t)flg;
        } else if (damaged == 3) packed[1] ^= 1; /* FCHECK */
        else if (damaged == 4) packed[offsets[1]-1] ^= 1; /* Adler32 */
        else if (damaged == 5) packed[2] = (packed[2] & ~6u) | 6u; /* reserved block type */
        else { /* Extra DEFLATE byte after BFINAL but before valid Adler32. */
            memmove(packed + offsets[1]-3, packed + offsets[1]-4, offsets[2]-offsets[1]+4);
            packed[offsets[1]-4] = 0;
            ++offsets[1]; ++offsets[2]; ++descriptor.compressed_size;
        }
        assert(!passport_font_get_bitmap(&glyph, out) && font_heap_live == 0);
        if (damaged == 6) { --offsets[1]; --offsets[2]; --descriptor.compressed_size; }
    }
    memcpy(packed, original, sizeof packed);
    assert(!passport_font_get_bitmap(&glyph, NULL));
    assert(passport_font_get_bitmap(&glyph, out) == out);
    assert(font_heap_live == 0 && font_heap_peak <= 512 && font_heap_largest_request <= 512);
    lv_draw_buf_destroy(out); assert(lv_mem_test() == LV_RESULT_OK);
    puts("Font blocks PASS: exact A8/4bpp, cross-block, part offset, short last block, malformed data/bounds, repeated decode, 512-byte heap cap, checksum/FCHECK/dictionary/trailing rejection.");
    return 0;
}
