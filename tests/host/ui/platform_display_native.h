/* Same screen clipping and buffer geometry as the platform; native ABI and
 * synchronous flush still differ from the32bit MCU/SPI/task execution. */
#pragma once
#include "lvgl.h"
#include "platform_display_production.inc"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint16_t platform_draw_first[240 * PASSPORT_DISPLAY_ROWS];
static uint16_t platform_draw_second[240 * PASSPORT_DISPLAY_ROWS];
static void native_platform_log(lv_log_level_t level, const char *text) {
    (void)level;
    fputs(text, stderr);
    if (strstr(text, "Allocating layer buffer failed")) {
        /* Production retries; a host diagnostic must fail promptly instead of
         * holding an unbounded warning stream in the test runner's memory. */
        lv_mem_monitor_t m; lv_mem_monitor(&m);
        fprintf(stderr, "NATIVE_LAYER_OOM peak=%zu free=%zu largest=%zu pool=%zu\n",
                m.max_used, m.free_size, m.free_biggest_size, m.total_size);
        abort();
    }
}
static lv_display_t *native_platform_display(lv_display_flush_cb_t flush) {
    lv_log_register_print_cb(native_platform_log);
    lv_display_t *d = lv_display_create(240, 320);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, platform_draw_first, platform_draw_second,
                           sizeof platform_draw_first, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d, flush);
#if SCREEN_CORNER_RADIUS > 0
    lv_obj_set_style_bg_color(lv_layer_bottom(), lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_COVER, 0);
    lv_display_add_event_cb(d, round_active_screen, LV_EVENT_SCREEN_LOADED, NULL);
    round_active_screen(NULL);
#endif
    return d;
}
