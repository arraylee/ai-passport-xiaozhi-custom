#include "lvgl.h"
#include "presentation/ui_game.h"
#include "presentation/bobo_art.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned exits, results;
static uint16_t buffer[240 * 20];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
static void exited(void *user) { (void)user; ++exits; }
static void result(int score, void *user) { (void)user; assert(score >= 0); ++results; }
static void step(void) { for (unsigned i=0; i<2; ++i) { lv_tick_inc(20); lv_timer_handler(); lv_obj_update_layout(lv_screen_active()); } }
static void key(ui_game_key_t k) { ui_game_key(k); step(); }
static bool has(lv_obj_t *o, const char *text) {
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(o); ++i) if (has(lv_obj_get_child(o, i), text)) return true;
    return false;
}
static bool is_obstacle(lv_obj_t *o) {
    if (!lv_obj_check_type(o, &lv_image_class)) return false;
    const void *src = lv_image_get_src(o);
    return src == &dodge_art_luggage || src == &dodge_art_robot ||
           src == &dodge_art_plant || src == &dodge_art_books;
}
static void test_rendered_acceleration(void) {
    ui_game_open(exited, NULL); key(UI_GAME_KEY_ENTER);
    lv_obj_t *screen = lv_screen_active(), *player = NULL;
    for (unsigned i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *o = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(o, &lv_image_class) && !is_obstacle(o)) player = o;
    }
    assert(player != NULL);
    // Verify positions produced by the actual 40 ms LVGL timer, beyond service
    // constants. The first 50 frames move 4 px each; frame 301 reaches 10 px.
    for (unsigned frame = 2; frame <= 400; ++frame) {
        lv_obj_t *tracked = NULL;
        for (unsigned i = 0; i < lv_obj_get_child_count(screen); ++i) {
            lv_obj_t *o = lv_obj_get_child(screen, i);
            if (!is_obstacle(o) || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) continue;
            int x = lv_obj_get_x(o);
            if (x >= 70) tracked = o;
            if (x <= 120 && x + 40 > 32 && lv_obj_get_y(o) + 12 == lv_obj_get_y(player))
                ui_game_key(lv_obj_get_y(player) == 90 ? UI_GAME_KEY_DOWN : UI_GAME_KEY_UP);
        }
        assert(tracked != NULL);
        int old_x = lv_obj_get_x(tracked);
        if (frame == 2) assert(old_x == 236);
        step();
        unsigned expected = frame <= 50 ? 4 : frame <= 100 ? 5 : frame <= 150 ? 6 :
                            frame <= 200 ? 7 : frame <= 250 ? 8 : frame <= 300 ? 9 : 10;
        assert(old_x - lv_obj_get_x(tracked) == (int)expected);
        assert(!has(screen, "再来一次"));
    }
    key(UI_GAME_KEY_EXIT);
}
int main(void) {
    lv_init(); lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *home = lv_screen_active();
    ui_game_set_result_cb(result, NULL);
    ui_game_open(exited, NULL); key(UI_GAME_KEY_DOWN); key(UI_GAME_KEY_ENTER);
    assert(exits == 1 && !ui_game_is_active() && lv_screen_active() == home);
    ui_game_open(exited, NULL); key(UI_GAME_KEY_ENTER); key(UI_GAME_KEY_ENTER);
    assert(has(lv_screen_active(), "休息一下"));
    for (unsigned i = 0; i < 400; ++i) step();
    assert(results == 0 && has(lv_screen_active(), "休息一下"));
    key(UI_GAME_KEY_DOWN); key(UI_GAME_KEY_ENTER);
    for (unsigned i = 0; i < 500 && !results; ++i) step();
    assert(results == 1 && has(lv_screen_active(), "再来一次"));
    for (unsigned i = 0; i < 100; ++i) step();
    assert(results == 1);
    key(UI_GAME_KEY_DOWN); key(UI_GAME_KEY_ENTER);
    assert(exits == 2 && lv_screen_active() == home);
    ui_game_open(exited, NULL); key(UI_GAME_KEY_ENTER); ui_game_pause_for_sleep();
    for (unsigned i = 0; i < 100; ++i) step();
    assert(results == 1 && has(lv_screen_active(), "休息一下"));
    key(UI_GAME_KEY_DOWN); key(UI_GAME_KEY_DOWN); key(UI_GAME_KEY_ENTER);
    assert(exits == 3 && !ui_game_is_active());
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 100; ++i) {
        ui_game_open(exited, NULL); key(UI_GAME_KEY_DOWN); key(UI_GAME_KEY_ENTER);
    }
    lv_mem_monitor(&after);
    assert(before.free_size == after.free_size && lv_mem_test() == LV_RESULT_OK);
    test_rendered_acceleration();
    assert(!ui_game_is_active() && lv_screen_active() == home);
    puts("game UI: rendered 40 ms acceleration, short returns, stable pause and 100 cycles passed");
}
