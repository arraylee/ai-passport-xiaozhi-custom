#include "lvgl.h"
#include "presentation/ui_profile.h"
#include "presentation/ui_licenses.h"
#include "presentation/ui_theme.h"
#include "presentation/font_notices.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_indev_t *input;
static lv_indev_state_t input_state = LV_INDEV_STATE_RELEASED;
static int input_step;
static unsigned home_actions;
static uint16_t draw_buffer[240 * 20], framebuffer[240 * 320];
static void read_input(lv_indev_t *device, lv_indev_data_t *data) {
    (void)device;
    data->state = input_state; data->key = LV_KEY_ENTER; data->enc_diff = input_step; input_step = 0;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) framebuffer[y * 240 + x] = *source++;
    lv_display_flush_ready(display);
}
static void settle(void) {
    for (unsigned i = 0; i < 3; ++i) { lv_tick_inc(5); lv_timer_handler(); lv_indev_read(input); }
    lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
    assert(lv_mem_test() == LV_RESULT_OK);
}
static bool label_is(lv_obj_t *object, const char *text) {
    if (lv_obj_check_type(object, &lv_label_class) && strcmp(lv_label_get_text(object), text) == 0) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (label_is(lv_obj_get_child(object, i), text)) return true;
    return false;
}
static lv_obj_t *control(const char *text) {
    lv_group_t *group = lv_group_get_default();
    for (unsigned i = 0; i < lv_group_get_obj_count(group); ++i) {
        lv_obj_t *object = lv_group_get_obj_by_index(group, i);
        if (label_is(object, text)) return object;
    }
    assert(false); return NULL;
}
static void focus(const char *text) {
    for (unsigned i = 0; i < 5; ++i) {
        if (label_is(lv_group_get_focused(lv_group_get_default()), text)) return;
        input_step = 1; lv_indev_read(input);
    }
    assert(false);
}
static void click(const char *text) {
    focus(text);
    input_state = LV_INDEV_STATE_PRESSED; lv_indev_read(input);
    input_state = LV_INDEV_STATE_RELEASED; lv_indev_read(input);
}
static void open_from_home(void) {
    input_state = LV_INDEV_STATE_PRESSED; lv_indev_read(input);
    assert(ui_profile_open_licenses());
    settle();
    input_state = LV_INDEV_STATE_RELEASED; lv_indev_read(input); settle();
}
static void act(const char *text) { click(text); settle(); }
static lv_obj_t *body(void) {
    lv_obj_t *screen = lv_screen_active();
    for (unsigned i = 0; i < lv_obj_get_child_count(screen); ++i) {
        lv_obj_t *object = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(object, &lv_label_class) && lv_obj_get_y(object) == 48) return object;
    }
    assert(false); return NULL;
}
static size_t without_linebreaks(const char *text, size_t size, char *out) {
    size_t written = 0;
    for (size_t i = 0; i < size; ++i) if (text[i] != '\n' && text[i] != '\r') out[written++] = text[i];
    out[written] = '\0'; return written;
}
static void verify_glyphs(lv_obj_t *object) {
    if (lv_obj_check_type(object, &lv_label_class)) {
        const unsigned char *p = (const unsigned char *)lv_label_get_text(object);
        const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
        while (*p) {
            uint32_t cp = *p++;
            if (cp >= 0xC0) {
                unsigned remaining = cp < 0xE0 ? 1 : cp < 0xF0 ? 2 : 3;
                cp &= remaining == 1 ? 31 : remaining == 2 ? 15 : 7;
                while (remaining--) cp = (cp << 6) | (*p++ & 63);
            }
            if (cp < 32) continue;
            lv_font_glyph_dsc_t glyph = {0};
            if (!lv_font_get_glyph_dsc(font, &glyph, cp, 0) || glyph.is_placeholder) {
                fprintf(stderr, "License UI missing glyph U+%04X\n", cp); abort();
            }
            if (cp >= 0x2E80 && cp <= 0x9FFF) {
                assert(glyph.resolved_font == &ui_font_caption || glyph.resolved_font == &ui_font_body ||
                       glyph.resolved_font == &ui_font_title);
                assert(((const lv_font_fmt_txt_dsc_t *)glyph.resolved_font->dsc)->bpp == 4);
            }
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) verify_glyphs(lv_obj_get_child(object, i));
}
static void verify_page(ui_licenses_view_t *view) {
    assert(ui_licenses_get_view(view));
    assert(view->end > view->offset && view->end <= strlen(passport_font_notices));
    assert(((unsigned char)passport_font_notices[view->offset] & 0xC0) != 0x80);
    assert(((unsigned char)passport_font_notices[view->end] & 0xC0) != 0x80);
    const char *visible = lv_label_get_text(body());
    assert(strlen(visible) < 1024);
    char actual[2048], expected[2048];
    size_t actual_size = without_linebreaks(visible, strlen(visible), actual);
    size_t expected_size = without_linebreaks(passport_font_notices + view->offset, view->end - view->offset, expected);
    assert(actual_size == expected_size && memcmp(actual, expected, actual_size) == 0);
    lv_point_t extent;
    lv_text_get_size(&extent, visible, &ui_font_caption, 0, 2, 216, LV_TEXT_FLAG_NONE);
    assert(extent.y <= 210);
    /* Each pre-wrapped line fits even with no secondary LVGL wrapping. */
    lv_text_get_size(&extent, visible, &ui_font_caption, 0, 2, 216, LV_TEXT_FLAG_EXPAND);
    assert(extent.x <= 216);
    verify_glyphs(lv_screen_active());
}
static void capture(const char *name) {
    const char *directory = getenv("LICENSE_UI_SCREENSHOT_DIR");
    if (!directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n240 320\n255\n");
    for (unsigned i = 0; i < 240 * 320; ++i) {
        uint16_t color = framebuffer[i];
        uint8_t rgb[] = {((color >> 11) & 31) * 255 / 31, ((color >> 5) & 63) * 255 / 63, (color & 31) * 255 / 31};
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
}
static void home_action(int item_id, void *user) { (void)item_id; (void)user; ++home_actions; }
int main(void) {
    lv_init();
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_group_t *home_group = lv_group_create(); lv_group_set_default(home_group);
    input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(input, read_input); lv_indev_set_group(input, home_group);
    lv_timer_pause(lv_indev_get_read_timer(input));
    lv_obj_t *home = lv_screen_active(); ui_profile_create(); ui_profile_set_on_action(home_action, NULL);
    ui_profile_create(); assert(lv_group_get_obj_count(home_group) == 2);
    assert(label_is(lv_group_get_obj_by_index(home_group, 0), "玩法"));
    assert(label_is(lv_group_get_obj_by_index(home_group, 1), "相册"));
    assert(!label_is(home, "关于") && !label_is(home, "上下选择 确定"));
    focus("玩法"); settle(); verify_glyphs(home); capture("profile-clean-footer");
    lv_obj_t *original_focus = lv_group_get_focused(home_group);
    assert(ui_profile_open_licenses());
    /* Screen/group do not change inside the encoder's release callback. */
    assert(ui_licenses_is_active() && lv_screen_active() == home && lv_group_get_default() == home_group);
    assert(!ui_licenses_open()); settle();
    assert(lv_screen_active() != home && lv_group_get_default() != home_group);
    assert(lv_indev_get_group(input) == lv_group_get_default());
    assert(lv_group_get_obj_count(home_group) == 2 && lv_group_get_obj_count(lv_group_get_default()) == 3);
    assert(lv_obj_has_state(control("上一页"), LV_STATE_DISABLED));
    ui_licenses_view_t view; verify_page(&view); capture("font-notices-first");
    size_t offsets[512], expected_offset = 0;
    unsigned pages = view.pages;
    assert(pages > 1 && pages < 512);
    const char *longest_url = NULL;
    size_t longest_url_bytes = 0;
    for (const char *url = strstr(passport_font_notices, "https://"); url; url = strstr(url + 8, "https://")) {
        size_t bytes = strcspn(url, " \n\r)");
        if (bytes > longest_url_bytes) { longest_url = url; longest_url_bytes = bytes; }
    }
    assert(longest_url && longest_url_bytes > 40);
    bool captured_url = false;
    for (unsigned page = 1; page <= pages; ++page) {
        verify_page(&view); assert(view.page == page && view.offset == expected_offset && view.pages == pages);
        offsets[page - 1] = view.offset; expected_offset = view.end;
        if (!captured_url && longest_url >= passport_font_notices + view.offset && longest_url < passport_font_notices + view.end) {
            capture("font-notices-long-url"); captured_url = true;
        }
        if (page < pages) act("下一页");
    }
    assert(expected_offset == strlen(passport_font_notices) && captured_url);
    assert(lv_obj_has_state(control("下一页"), LV_STATE_DISABLED)); capture("font-notices-last");
    for (unsigned page = pages; page > 1; --page) {
        act("上一页"); verify_page(&view); assert(view.page == page - 1 && view.offset == offsets[page - 2]);
    }
    assert(lv_obj_has_state(control("上一页"), LV_STATE_DISABLED));
    click("返回"); assert(lv_screen_active() != home); settle();
    assert(!ui_licenses_is_active() && lv_screen_active() == home);
    assert(lv_group_get_default() == home_group && lv_indev_get_group(input) == home_group);
    assert(lv_group_get_focused(home_group) == original_focus && home_actions == 0);
    /* The physical long-OK release can arrive before the queued screen open.
     * It must not click the still-visible homepage tile in that interval. */
    input_state = LV_INDEV_STATE_PRESSED; lv_indev_read(input);
    assert(ui_profile_open_licenses());
    input_state = LV_INDEV_STATE_RELEASED; lv_indev_read(input);
    assert(home_actions == 0 && lv_screen_active() == home);
    settle(); assert(ui_licenses_is_active() && lv_screen_active() != home);
    act("返回"); assert(home_actions == 0 && lv_screen_active() == home);
    unsigned group_count = lv_group_get_count();
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 100; ++i) {
        open_from_home(); act("返回");
        assert(!ui_licenses_is_active() && lv_screen_active() == home);
        assert(lv_group_get_focused(home_group) == original_focus && lv_group_get_count() == group_count);
    }
    lv_mem_monitor(&after); assert(after.free_size == before.free_size);
    /* Closing while a key is still held cannot click the restored homepage tile. */
    open_from_home(); focus("返回"); input_state = LV_INDEV_STATE_PRESSED; lv_indev_read(input);
    ui_licenses_close(); settle();
    input_state = LV_INDEV_STATE_RELEASED; lv_indev_read(input); settle();
    assert(!ui_licenses_is_active() && lv_screen_active() == home && home_actions == 0);
    assert(ui_licenses_open()); ui_licenses_close(); settle(); assert(!ui_licenses_is_active());
    /* Deleting the owner safely cancels or closes its reader and clock. */
    open_from_home(); lv_obj_delete(home); settle();
    assert(!ui_licenses_is_active() && lv_group_get_count() == group_count);
    lv_obj_t *fallback = lv_screen_active();
    home = lv_obj_create(NULL); lv_screen_load(home); lv_obj_delete(fallback);
    ui_profile_create(); settle(); assert(lv_group_get_obj_count(home_group) == 2);
    open_from_home(); act("返回"); assert(lv_screen_active() == home);
    assert(lv_mem_test() == LV_RESULT_OK && home_actions == 0);
    lv_mem_monitor(&after);
    printf("License UI PASS: %zu original bytes, %u pages forward/back, native encoder release, first/last/%zu-byte URL, all glyphs, restored focus, 100 no-leak cycles, owner lifecycle.\n", strlen(passport_font_notices), pages, longest_url_bytes);
    printf("LVGL memory: current=%zu peak=%zu pool=%zu; repeated-cycle free=%zu.\n",
           after.total_size - after.free_size, after.max_used, after.total_size, before.free_size);
    return 0;
}
