#include "presentation/passport_font_blocks.h"
#include "font_heap_cap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
LV_FONT_DECLARE(lv_font_cn_24);
LV_FONT_DECLARE(ui_font_title);
int main(void) {
    lv_init();
    const passport_font_block_dsc_t *first = lv_font_cn_24.dsc;
    uint8_t *oracle = malloc(first->raw_size); assert(oracle);
    /* Independent system zlib is the pixel oracle only, never production decoding. */
    for (uint32_t block = 0; block < first->block_count; ++block) {
        uint32_t start = block * PASSPORT_FONT_BLOCK_BYTES;
        uLongf size = first->raw_size - start;
        if (size > PASSPORT_FONT_BLOCK_BYTES) size = PASSPORT_FONT_BLOCK_BYTES;
        uLong consumed = first->block_offsets[block+1] - first->block_offsets[block];
        uLong supplied = consumed;
        assert(uncompress2(oracle + start, &size, first->compressed + first->block_offsets[block], &consumed) == Z_OK);
        assert(consumed == supplied);
    }
    unsigned decoded = 0, cross_block = 0;
    for (const lv_font_t *part = &lv_font_cn_24; part; part = part->fallback) {
        const passport_font_block_dsc_t *dsc = part->dsc;
        assert(dsc->raw_size == first->raw_size && part->get_glyph_bitmap == passport_font_get_bitmap);
        for (uint32_t id = 1; id < dsc->glyph_count; ++id) {
            const lv_font_fmt_txt_glyph_dsc_t *m = &dsc->fmt.glyph_dsc[id];
            if (!m->box_w || !m->box_h) continue;
            lv_draw_buf_t *out = lv_draw_buf_create(m->box_w, m->box_h, LV_COLOR_FORMAT_A8, 0); assert(out);
            lv_font_glyph_dsc_t glyph = {.resolved_font=part,.gid.index=id,.box_w=m->box_w,.box_h=m->box_h};
            assert(passport_font_get_bitmap(&glyph,out) == out && font_heap_live == 0);
            uint32_t begin = dsc->part_base + m->bitmap_index, pixels = (uint32_t)m->box_w*m->box_h;
            for (uint32_t i=0; i<pixels; ++i) {
                uint8_t value = oracle[begin+i/2u];
                value = i & 1u ? value & 15u : value >> 4;
                assert(out->data[(i/m->box_w)*out->header.stride+i%m->box_w] == value*17u);
            }
            cross_block += begin/PASSPORT_FONT_BLOCK_BYTES != (begin+(pixels-1u)/2u)/PASSPORT_FONT_BLOCK_BYTES;
            ++decoded; lv_draw_buf_destroy(out);
        }
    }
    /* Reproduce the actual final labels through the real fast/fallback lookup. */
    const uint32_t labels[] = {0x81ea,0x7136,0x5bf9,0x8bdd,0x5e38,0x89c1,0x56de,0x590d,0x9f9f};
    for (unsigned i=0;i<sizeof labels/sizeof *labels;++i) {
        lv_font_glyph_dsc_t glyph={0};
        assert(lv_font_get_glyph_dsc(&ui_font_title,&glyph,labels[i],0) && !glyph.is_placeholder);
        lv_draw_buf_t *out=lv_draw_buf_create(glyph.box_w,glyph.box_h,LV_COLOR_FORMAT_A8,0);assert(out);
        assert(lv_font_get_glyph_bitmap(&glyph,out)==out);
        bool visible=false;
        for(unsigned y=0;y<glyph.box_h;y++)for(unsigned x=0;x<glyph.box_w;x++)
            if(out->data[y*out->header.stride+x])visible=true;
        assert(visible && font_heap_live==0);lv_draw_buf_destroy(out);
    }
    free(oracle);
    assert(decoded > 6000 && cross_block > 0 && font_heap_peak <= 512 && font_heap_largest_request <= 512);
    assert(font_heap_live == 0 && lv_mem_test() == LV_RESULT_OK);
    printf("All rare nickname glyphs: %u pixel-exact 4bpp/A8, %u cross-block, heap cap512, production decoder; 自然对话/常见回复/龟 visible through final font lookup.\n", decoded,cross_block);
    return 0;
}
