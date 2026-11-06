#include "lvgl.h"
#include "presentation/ui_profile.h"
#include "presentation/ui_album.h"
#include "presentation/ui_theme.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "battery_pixels.h"

static uint16_t buffer[240 * 20];
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    collect_gauge(area,pixels); lv_display_flush_ready(display);
}
static void settle(void) {
    for (unsigned i = 0; i < 3; ++i) { lv_tick_inc(20); lv_timer_handler(); }
    lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
    assert(lv_mem_test() == LV_RESULT_OK);
}
static bool label_is(lv_obj_t *object, const char *text) {
    if (lv_obj_check_type(object, &lv_label_class) && !strcmp(lv_label_get_text(object), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (label_is(lv_obj_get_child(object, i), text)) return true;
    return false;
}
static void bounds(lv_obj_t *object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t box; lv_obj_get_coords(object, &box);
        assert(box.x1 >= 0 && box.y1 >= 0 && box.x2 < 240 && box.y2 < 320);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) bounds(lv_obj_get_child(object, i));
}
static lv_obj_t *find_size(lv_obj_t *root, int w, int h) {
    for (unsigned i=0;i<lv_obj_get_child_count(root);i++) {
        lv_obj_t *o=lv_obj_get_child(root,i);
        if(lv_obj_get_width(o)==w && lv_obj_get_height(o)==h) return o;
    }
    return NULL;
}
static ui_album_action_t click(int index) {
    assert(ui_album_button(index, UI_ALBUM_PRESS) == UI_ALBUM_NONE);
    ui_album_action_t result = ui_album_button(index, UI_ALBUM_CLICK); settle(); return result;
}
int main(void) {
    lv_init();
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof buffer, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_group_t *group = lv_group_create(); lv_group_set_default(group);
    lv_obj_t *home = lv_screen_active(); ui_profile_create(); settle();
    assert(lv_color_eq(lv_obj_get_style_bg_color(home, 0), lv_color_hex(0x0E1512)));
    assert(lv_group_get_obj_count(group) == 2);
    assert(!label_is(home, "上下选择 确定") && !label_is(home, "关于"));
    assert(label_is(home, "探索AI的\n无限可能"));
    // Initial/unknown SOC must not appear as an actual empty or full battery.
    lv_obj_t *bt=find_size(home,14,20), *icon=find_size(home,24,14);
    assert(bt && icon);
    lv_obj_t *slash=lv_obj_get_child(bt,1);
    assert(!label_is(home,"--%") && !label_is(home,"100%")); assert_gauge(-1,0);
    for(int percent=0;percent<=100;percent++) {
        ui_profile_set_battery(percent,3); settle(); assert_gauge(percent,3);
        char text[16]; snprintf(text,sizeof text,"%d%%",percent); assert(!label_is(home,text)); bounds(home);
    }
    ui_profile_set_battery(-1,0); settle(); assert_gauge(-1,0);
    assert(!lv_obj_has_flag(slash,LV_OBJ_FLAG_HIDDEN));
    ui_profile_set_ble_connected(true); lv_tick_inc(1100); settle();
    assert(lv_obj_has_flag(slash,LV_OBJ_FLAG_HIDDEN));
    assert(lv_color_eq(lv_obj_get_style_line_color(lv_obj_get_child(bt,0),0),lv_color_hex(0x79BFFF)));
    ui_profile_set_ble_connected(false); lv_tick_inc(1100); settle();
    assert(!lv_obj_has_flag(slash,LV_OBJ_FLAG_HIDDEN));
    ui_profile_set_name("博博"); ui_profile_set_title("旅行伙伴"); ui_profile_set_intro("今天去博物馆"); settle();
    assert(label_is(home, "博博") && label_is(home, "旅行伙伴") && label_is(home, "今天去博物馆"));
    ui_profile_set_intro(""); settle(); assert(label_is(home, "探索AI的\n无限可能")); bounds(home);
    unsigned home_children = lv_obj_get_child_count(home);
    ui_album_no_photo(home); settle();
    assert(ui_album_active() && label_is(home, "暂无照片，请通过小程序上传")); bounds(home);
    assert(ui_album_button(2, UI_ALBUM_CLICK) == UI_ALBUM_NONE && ui_album_active());
    assert(click(0) == UI_ALBUM_NONE && ui_album_active());
    assert(click(2) == UI_ALBUM_DISMISSED && !ui_album_active());
    assert(lv_obj_get_child_count(home) == home_children);

    lv_obj_t *photo = lv_obj_create(NULL); lv_screen_load(photo); ui_album_attach(photo); settle();
    assert(ui_album_active() && lv_obj_get_child_count(photo) == 0);
    assert(ui_album_button(2, UI_ALBUM_CLICK) == UI_ALBUM_NONE);
    assert(lv_obj_get_child_count(photo) == 0); // delayed homepage click must not open menu
    assert(click(2) == UI_ALBUM_NONE);
    assert(label_is(photo, "返回首页") && label_is(photo, "取消")); bounds(photo);
    assert(click(1) == UI_ALBUM_NONE); assert(click(2) == UI_ALBUM_NONE); // Cancel
    assert(lv_obj_get_child_count(photo) == 0 && ui_album_active());
    assert(click(2) == UI_ALBUM_NONE); assert(click(0) == UI_ALBUM_NONE);
    assert(click(0) == UI_ALBUM_NONE); assert(click(2) == UI_ALBUM_RETURN_HOME);
    lv_screen_load(home); lv_obj_delete(photo); settle(); assert(!ui_album_active());

    // Warm the one-second profile clock formatter before measuring modal lifetime.
    lv_tick_inc(1100); settle();
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 110; ++i) {
        ui_album_no_photo(home); settle(); assert(click(2) == UI_ALBUM_DISMISSED);
        photo = lv_obj_create(NULL); lv_screen_load(photo); ui_album_attach(photo);
        click(2); click(1); click(2); click(2); assert(click(2) == UI_ALBUM_RETURN_HOME);
        lv_screen_load(home); lv_obj_delete(photo); settle(); assert(!ui_album_active());
        // Stabilize allocator layout, then require 100 cycles with no growth.
        if (i == 9) lv_mem_monitor(&before);
        if (i >= 10) {
            lv_mem_monitor(&after);
            // TLSF free-block headers vary with layout; compare live allocations.
            assert(after.used_cnt == before.used_cnt);
            assert(after.total_size - after.free_size <= before.total_size - before.free_size);
        }
    }
    lv_mem_monitor(&after);
    assert(after.used_cnt == before.used_cnt &&
           after.total_size - after.free_size == before.total_size - before.free_size);
    // A new incoming photo replaces an open notice without leaving a stale owner.
    ui_album_no_photo(home); photo = lv_obj_create(NULL); lv_screen_load(photo); ui_album_attach(photo);
    assert(lv_obj_get_child_count(home) == home_children);
    lv_obj_t *next = lv_obj_create(NULL); lv_screen_load(next); ui_album_attach(next);
    lv_obj_delete(photo); assert(ui_album_active());
    lv_screen_load(home); lv_obj_delete(next); settle(); assert(!ui_album_active());
    assert(lv_group_get_obj_count(group) == 2);
    printf("Profile/album PASS: original green, personal info/default intro, two homepage controls, empty notice, clean photo, OK/up/down/cancel, delayed-click isolation, owner replacement, 100 no-leak cycles. LVGL peak=%zu/%zu.\n", after.max_used, after.total_size);
    return 0;
}
