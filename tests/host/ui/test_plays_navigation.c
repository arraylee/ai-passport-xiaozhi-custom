#include "lvgl.h"
#include "presentation/ui_plays.h"
#include "presentation/ui_theme.h"
#include "services/answer_content.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint16_t buffer[240 * 20];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
static unsigned answer_labels;
static void check_menu_rows(void) {
    lv_obj_t *screen = lv_screen_active(); unsigned rows = 0;
    lv_obj_update_layout(screen);
    for (unsigned i=0; i<lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *row=lv_obj_get_child(screen,i);
        if (!lv_obj_check_type(row,&lv_obj_class) || !lv_obj_get_child_count(row)) continue;
        lv_obj_t *label=lv_obj_get_child(row,0);
        if (!lv_obj_check_type(label,&lv_label_class)) continue;
        assert(lv_obj_get_height(row)>=44);
        assert(lv_obj_get_style_text_font(label,0)->line_height>=22);
        rows++;
    }
    assert(rows==4);
}
static void check_book(lv_obj_t *obj, const char *answer) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t a, p; lv_obj_get_coords(obj, &a); lv_obj_get_coords(lv_obj_get_parent(obj), &p);
        if (a.x1 < p.x1 || a.x2 > p.x2 || a.y1 < p.y1 || a.y2 > p.y2)
            fprintf(stderr, "Clipped: %s\n", lv_label_get_text(obj));
        assert(a.x1 >= p.x1 && a.x2 <= p.x2 && a.y1 >= p.y1 && a.y2 <= p.y2);
        if (lv_obj_get_style_text_font(obj, 0) == &ui_font_answer && answer) {
            char plain[128]; size_t n = 0;
            for (const char *s = lv_label_get_text(obj); *s; ++s) if (*s != '\n') plain[n++] = *s;
            plain[n] = 0; assert(strcmp(plain, answer) == 0);
            assert(lv_obj_get_height(obj) <= 65); ++answer_labels;
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(obj); ++i) check_book(lv_obj_get_child(obj, i), answer);
}

static unsigned exits, selections;
static ui_play_id_t selected_play;
static bool choose(ui_play_id_t play, void *user) {
    (void)user; assert(play <= UI_PLAY_CONNECT); selected_play = play; selections++; return true;
}
static void exited(void *user) { (void)user; exits++; }
int main(void) {
    lv_init(); lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *home = lv_screen_active();
    ui_plays_open(choose, exited, NULL);
    const ui_play_id_t expected[] = {UI_PLAY_XIAOZHI, UI_PLAY_ANSWER, UI_PLAY_CLICKER,
                                   UI_PLAY_DODGE, UI_PLAY_CONNECT};
    for (unsigned i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
        ui_plays_menu_key(UI_PLAY_OK);
        assert(selected_play == expected[i]);
        ui_plays_menu(); check_menu_rows();
        ui_plays_menu_key(UI_PLAY_DOWN);
    }
    ui_plays_menu_key(UI_PLAY_OK); // Exactly five plays plus Return Home.
    assert(selections == 5 && exits == 1 && lv_screen_active() == home);
    exits = selections = 0;
    ui_plays_open(choose, exited, NULL);
    check_menu_rows();
    for (int i = 0; i < 4; i++) ui_plays_menu_key(UI_PLAY_DOWN);
    ui_plays_menu_key(UI_PLAY_OK);
    assert(exits == 0 && selections == 1 && selected_play == UI_PLAY_CONNECT);
    ui_plays_menu(); // Returning from QR retains the selected connection entry.
    lv_obj_update_layout(lv_screen_active()); check_book(lv_screen_active(), NULL);
    ui_plays_menu_key(UI_PLAY_DOWN); ui_plays_menu_key(UI_PLAY_OK);
    assert(exits == 1 && selections == 1 && lv_screen_active() == home);
    ui_plays_open(choose, exited, NULL);
    // Up from the first play wraps directly to Return, not the connection action.
    ui_plays_menu_key(UI_PLAY_UP); ui_plays_menu_key(UI_PLAY_DOWN);
    ui_plays_menu_key(UI_PLAY_OK); assert(selections == 2 && selected_play == UI_PLAY_XIAOZHI);
    ui_plays_menu(); ui_plays_menu_key(UI_PLAY_DOWN);
    ui_plays_menu_key(UI_PLAY_OK); assert(selected_play == UI_PLAY_ANSWER);
    ab_session_view_t book = {.state = AB_STATE_IDLE};
    for (unsigned state = AB_STATE_IDLE; state <= AB_STATE_ANSWER; ++state) {
        book.state = (ab_state_t)state; book.answer_ready = state == AB_STATE_ANSWER;
        book.deadline_ms = 2900;
        for (unsigned i = 0; i < (book.answer_ready ? answer_text_count : 1); ++i) {
            book.answer_index = i; ui_plays_answer(&book, 600, 2500);
            lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
            check_book(lv_screen_active(), book.answer_ready ? answer_texts[i] : NULL);
        }
    }
    assert(answer_labels == answer_text_count);
    lv_mem_monitor_t first, last;
    book.state = AB_STATE_IDLE; ui_plays_answer(&book, 0, 3000); lv_refr_now(NULL);
    // Warm one complete cycle: freeing the differently sized menu rows can
    // coalesce allocator holes. The next 100 cycles must then remain stable.
    for (unsigned i = 0; i <= 100; ++i) {
        book.state = AB_STATE_LISTENING; ui_plays_answer(&book, 500, 3200); lv_refr_now(NULL);
        book.state = AB_STATE_ANSWER; ui_plays_answer(&book, 0, 4000); lv_refr_now(NULL);
        book.state = AB_STATE_IDLE; ui_plays_answer(&book, 0, 5000); lv_refr_now(NULL);
        if (i == 0) lv_mem_monitor(&first);
    }
    lv_mem_monitor(&last);
    assert(first.free_size == last.free_size);
    ui_click_view_t view = {.status = UI_CLICK_READY, .name = "AI PASSPORT Click"};
    ui_plays_clicker(&view);
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_COMPUTER_MODE);
    assert(ui_plays_clicker_key(UI_PLAY_UP) == UI_CLICK_ACTION_PREV);
    assert(ui_plays_clicker_key(UI_PLAY_DOWN) == UI_CLICK_ACTION_NEXT);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    // A connection refresh while the menu is open must preserve its selection.
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker(&view);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_PLAY);
    ui_plays_clicker_key(UI_PLAY_OK); ui_plays_clicker_key(UI_PLAY_DOWN);
    ui_plays_clicker_key(UI_PLAY_DOWN);
    ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_END);
    ui_plays_clicker_key(UI_PLAY_OK);
    for (int i = 0; i < 5; ++i) ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    // The visible cancel button is the default and returns via short OK.
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    ui_plays_clicker_key(UI_PLAY_UP); ui_plays_clicker(&view);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_CHANGE_COMPUTER);
    ui_plays_clicker_key(UI_PLAY_OK);
    for (int i = 0; i < 6; ++i) ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_EXIT);
    ui_plays_close(); assert(exits == 2);
    assert(lv_mem_test() == LV_RESULT_OK);
    puts("plays navigation: all short-press paths passed");
}
