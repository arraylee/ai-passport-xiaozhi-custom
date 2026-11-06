#ifndef OVERLAY_TEST_DISPLAY_H
#define OVERLAY_TEST_DISPLAY_H
#include "lvgl.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t overlay_frame[240 * 320], overlay_draw[240 * 20];
static unsigned overlay_flushes, overlay_warnings;
static void overlay_log(lv_log_level_t level, const char *text) {
    (void)level;
    if (overlay_warnings++ < 8) { fputs(text, stderr); fflush(stderr); }
}
static void overlay_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    uint16_t *source = (uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 240 && y >= 0 && y < 320);
            overlay_frame[y * 240 + x] = *source++;
        }
    ++overlay_flushes;
    lv_display_flush_ready(display);
}
static lv_obj_t *overlay_setup(void) {
    lv_init(); lv_log_register_print_cb(overlay_log);
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, overlay_draw, NULL, sizeof overlay_draw, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, overlay_flush);
    lv_obj_t *home = lv_screen_active();
    lv_obj_set_style_bg_color(home, lv_color_hex(0x123456), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_t *label = lv_label_create(home);
    lv_label_set_text(label, "retained screen"); lv_obj_center(label);
    lv_refr_now(display);
    assert(overlay_flushes);
    return home;
}
static void overlay_step(uint32_t elapsed) {
    lv_tick_inc(elapsed); lv_timer_handler(); lv_refr_now(NULL);
    assert(lv_mem_test() == LV_RESULT_OK);
}
static unsigned overlay_timer_count(void) {
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) ++count;
    return count;
}
static lv_obj_t *overlay_find_label(lv_obj_t *object, const char *text) {
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = overlay_find_label(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}
static void overlay_fullscreen(lv_obj_t *object) {
    lv_obj_update_layout(object);
    lv_area_t area; lv_obj_get_coords(object, &area);
    assert(area.x1 == 0 && area.y1 == 0 && area.x2 == 239 && area.y2 == 319);
}
#endif
