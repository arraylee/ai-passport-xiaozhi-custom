#pragma once
#include "presentation/ui_theme.h"
#include <assert.h>
// Capture only the actual 24×14 status-bar pixels flushed by the native display.
static uint16_t gauge_pixels[14][24];
static void collect_gauge(const lv_area_t *area, const uint8_t *data) {
    const uint16_t *src = (const uint16_t *)data;
    int stride = area->x2 - area->x1 + 1;
    for (int y=14; y<28; ++y) {
        if (y<area->y1 || y>area->y2) continue;
        for (int x=204; x<228; ++x)
            if (x>=area->x1 && x<=area->x2) gauge_pixels[y-14][x-204]=src[(y-area->y1)*stride+x-area->x1];
    }
}
static void assert_gauge(int percent, int level) {
    uint16_t bg=lv_color_to_u16(lv_obj_get_style_bg_color(lv_screen_active(), 0));
    uint16_t ink=lv_color_to_u16(lv_color_hex(level==0?UI_MUTED:level==1?0xE0463A:level==2?0xF2C14E:UI_INK));
    assert(gauge_pixels[7][0]==ink && gauge_pixels[7][22]==ink);
    int width=percent>0?(18*percent+99)/100:0;
    for (int x=2;x<20;++x) {
        bool filled=percent<0 ? (x>=7 && x<=14) : (x-2<width);
        assert(gauge_pixels[7][x]==(filled?ink:bg));
    }
}
