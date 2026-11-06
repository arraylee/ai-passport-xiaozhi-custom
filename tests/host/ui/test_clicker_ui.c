#include "lvgl.h"
#include "presentation/ui_plays.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint16_t draw_buffer[240 * 20], frame[240 * 320];
static void flush(lv_display_t *display, const lv_area_t *a, uint8_t *pixels) {
    const uint16_t *p = (const uint16_t *)pixels;
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x) frame[y * 240 + x] = *p++;
    lv_display_flush_ready(display);
}
static bool contains(lv_obj_t *object, const char *text) {
    if (lv_obj_check_type(object, &lv_label_class) &&
        strstr(lv_label_get_text(object), text)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (contains(lv_obj_get_child(object, i), text)) return true;
    return false;
}
static void check_bounds(lv_obj_t *object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t a, p;
        lv_obj_get_coords(object, &a); lv_obj_get_coords(lv_obj_get_parent(object), &p);
        if (a.x1 < p.x1 || a.y1 < p.y1 || a.x2 > p.x2 || a.y2 > p.y2)
            fprintf(stderr, "Clipped clicker label: %s\n", lv_label_get_text(object));
        assert(a.x1 >= p.x1 && a.y1 >= p.y1 && a.x2 <= p.x2 && a.y2 <= p.y2);
        assert(a.x1 >= 0 && a.y1 >= 0 && a.x2 < 240 && a.y2 < 320);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        check_bounds(lv_obj_get_child(object, i));
}
static void render(const char *directory, const char *name) {
    lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
    check_bounds(lv_screen_active());
    if (!directory) return;
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n240 320\n255\n");
    for (unsigned i = 0; i < 240 * 320; ++i) {
        uint16_t c = frame[i];
        uint8_t rgb[] = {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
}
int main(int argc, char **argv) {
    lv_init(); lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    const char *directory = argc > 1 ? argv[1] : NULL;
    ui_plays_open(NULL, NULL, NULL); render(directory, "plays-names");
    assert(contains(lv_screen_active(), "蓝牙翻页笔"));
    assert(contains(lv_screen_active(), "小智 AI 对话"));
    assert(contains(lv_screen_active(), "障碍躲避游戏"));
    assert(!contains(lv_screen_active(), "随身宠物"));
    for (unsigned i=0;i<4;i++) ui_plays_menu_key(UI_PLAY_DOWN);
    assert(contains(lv_screen_active(), "障碍躲避游戏"));

    ui_click_view_t view = {.status = UI_CLICK_PAIRING, .name = "AI PASSPORT 32D7DC"};
    ui_plays_clicker(&view);
    assert(contains(lv_screen_active(), "安卓刷视频"));
    render(directory, "clicker-purpose");
    ui_plays_clicker(&view); // Status refresh cannot skip purpose selection.
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_COMPUTER_MODE);
    render(directory, "clicker-pairing");
    assert(contains(lv_screen_active(), "请连接电脑蓝牙："));
    assert(!contains(lv_screen_active(), "请连接手机蓝牙："));
    assert(contains(lv_screen_active(), "AI PASSPORT 32D7DC"));
    assert(contains(lv_screen_active(), "Mac：系统设置 / 蓝牙"));
    assert(contains(lv_screen_active(), "Win：设置 / 蓝牙和设备"));
    assert(contains(lv_screen_active(), "添加设备"));
    assert(!contains(lv_screen_active(), "已连接，可以翻页"));

    view.status = UI_CLICK_READY; view.sent = 27;
    ui_plays_clicker(&view); render(directory, "clicker-connected");
    assert(contains(lv_screen_active(), "上键：上一页"));
    assert(contains(lv_screen_active(), "下键：下一页"));
    assert(!contains(lv_screen_active(), "添加设备"));
    assert(contains(lv_screen_active(), "27"));

    ui_plays_clicker_key(UI_PLAY_OK); ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    view.status = UI_CLICK_PAIRING; ui_plays_clicker(&view);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_PLAY);
    render(directory, "clicker-disconnected");
    assert(contains(lv_screen_active(), "添加设备"));
    assert(!contains(lv_screen_active(), "已连接，可以翻页"));

    view.status = UI_CLICK_READY; ui_plays_clicker(&view);
    ui_plays_clicker_key(UI_PLAY_OK); render(directory, "clicker-actions-page1");
    assert(contains(lv_screen_active(), "开始演示"));
    assert(contains(lv_screen_active(), "从第一页开始"));
    assert(contains(lv_screen_active(), "选择其他用途"));
    assert(!contains(lv_screen_active(), "开始或结束"));
    for (unsigned i = 0; i < 3; ++i) ui_plays_clicker_key(UI_PLAY_DOWN);
    render(directory, "clicker-actions-page2");
    assert(contains(lv_screen_active(), "暂时隐藏画面"));
    assert(contains(lv_screen_active(), "再按一次恢复"));
    assert(contains(lv_screen_active(), "回到编辑界面"));
    ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_BLANK);
    ui_plays_clicker_key(UI_PLAY_OK);
    for (unsigned i = 0; i < 5; ++i) ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    render(directory, "clicker-change-computer");
    assert(contains(lv_screen_active(), "AI PASSPORT 32D7DC"));
    assert(!contains(lv_screen_active(), "AI PASSPORT Click"));
    assert(contains(lv_screen_active(), "旧设备若自动重连"));
    assert(contains(lv_screen_active(), "取消，返回菜单"));
    assert(contains(lv_screen_active(), "上下选择   确定执行"));
    view.status = UI_CLICK_PAIRING; ui_plays_clicker(&view);
    assert(contains(lv_screen_active(), "断开并重新配对"));
    /* Default selection must cancel on a short press, without Bluetooth work. */
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    assert(contains(lv_screen_active(), "翻页笔操作"));
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_UP) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_DOWN) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    assert(contains(lv_screen_active(), "翻页笔操作"));
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_BACK) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    assert(ui_plays_clicker_key(UI_PLAY_DOWN) == UI_CLICK_ACTION_NONE);
    view.status = UI_CLICK_READY; ui_plays_clicker(&view);
    render(directory, "clicker-change-computer-confirm");
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_CHANGE_COMPUTER);
    view.status = UI_CLICK_SWITCHING; ui_plays_clicker(&view); render(directory, "clicker-switching");
    assert(contains(lv_screen_active(), "正在断开旧设备"));
    view.status = UI_CLICK_PAIRING; ui_plays_clicker(&view); render(directory, "clicker-new-pairing");
    assert(contains(lv_screen_active(), "旧设备请先断开或关闭蓝牙"));

    for (unsigned status = UI_CLICK_OFF; status <= UI_CLICK_SWITCHING; ++status) {
        view.status = (ui_click_status_t)status; ui_plays_clicker(&view); render(NULL, NULL);
    }
    // Switch through the visible menu; phone actions never expose PPT keys.
    ui_plays_clicker_key(UI_PLAY_OK); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_PHONE_MODE);
    view.phone_mode = true; view.status = UI_CLICK_PAIRING;
    ui_plays_clicker(&view); render(directory, "clicker-phone-pairing");
    assert(contains(lv_screen_active(), "请连接手机蓝牙："));
    assert(!contains(lv_screen_active(), "请连接电脑蓝牙："));
    assert(!contains(lv_screen_active(), "安卓：在系统蓝牙中配对"));
    assert(contains(lv_screen_active(), "请先开启辅助触控"));
    assert(contains(lv_screen_active(), "同时开启执行触控手势"));
    view.status = UI_CLICK_READY; ui_plays_clicker(&view); render(directory, "clicker-phone-ready");
    assert(contains(lv_screen_active(), "已连接，拖动模式"));
    assert(!contains(lv_screen_active(), "滚轮"));
    // BLE readiness cannot prove that the user enabled the iOS prerequisite.
    assert(contains(lv_screen_active(), "请先开启辅助触控"));
    assert(contains(lv_screen_active(), "上键：上一条"));
    assert(contains(lv_screen_active(), "下键：下一条"));
    ui_plays_clicker_key(UI_PLAY_OK); render(directory, "clicker-phone-actions");
    assert(contains(lv_screen_active(), "执行触控手势"));
    assert(contains(lv_screen_active(), "测试指针"));
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_POINTER_TEST);
    ui_plays_clicker_key(UI_PLAY_OK);
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    render(directory, "clicker-phone-actions-page2");
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_REVERSE_SCROLL);
    view.scroll_reversed = true; ui_plays_clicker(&view);
    assert(ui_plays_clicker_key(UI_PLAY_UP) == UI_CLICK_ACTION_PREV);
    assert(ui_plays_clicker_key(UI_PLAY_DOWN) == UI_CLICK_ACTION_NEXT);
    ui_plays_clicker_key(UI_PLAY_OK); render(directory, "clicker-phone-reversed");
    assert(contains(lv_screen_active(), "选择其他用途"));
    assert(!contains(lv_screen_active(), "开始演示"));
    // Last partial page and wrap must remain in bounds.
    ui_plays_clicker_key(UI_PLAY_UP); render(directory, "clicker-phone-exit");
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_EXIT);
    ui_plays_clicker_key(UI_PLAY_DOWN); // wrap to Continue
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    ui_plays_clicker_key(UI_PLAY_OK); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_COMPUTER_MODE);
    view.phone_mode = false; ui_plays_clicker(&view); render(NULL, NULL);
    ui_plays_clicker_key(UI_PLAY_OK); ui_plays_clicker_key(UI_PLAY_UP);
    render(directory, "clicker-actions-page3");
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_EXIT);
    ui_plays_clicker_key(UI_PLAY_BACK);
    /* Warm full cycles after menu teardown so freed row blocks can coalesce.
     * Neither retained bytes nor the live object tree may grow afterwards. */
    for (unsigned i = 0; i < 2; ++i) {
        view.status = UI_CLICK_READY; ui_plays_clicker(&view); render(NULL, NULL);
        view.status = UI_CLICK_PAIRING; ui_plays_clicker(&view); render(NULL, NULL);
    }
    lv_mem_monitor_t before, after; lv_mem_monitor(&before);
    for (unsigned i = 0; i < 100; ++i) {
        view.status = UI_CLICK_READY; ui_plays_clicker(&view); render(NULL, NULL);
        view.status = UI_CLICK_PAIRING; ui_plays_clicker(&view); render(NULL, NULL);
    }
    lv_mem_monitor(&after); fprintf(stderr, "clicker memory before=%zu after=%zu\n", before.free_size, after.free_size); assert(before.free_size == after.free_size);
    ui_plays_close();
    // Re-entering always offers a purpose, even after using computer mode.
    ui_plays_open(NULL, NULL, NULL); ui_plays_clicker(&view);
    assert(contains(lv_screen_active(), "安卓刷视频"));
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_TOUCH_MODE);
    assert(contains(lv_screen_active(), "安卓模式 · 连接步骤"));
    assert(contains(lv_screen_active(), "请连接手机蓝牙："));
    assert(!contains(lv_screen_active(), "辅助触控"));
    render(directory, "clicker-android-pairing");
    view.phone_mode=true; view.touch_mode=true; view.status=UI_CLICK_READY;
    ui_plays_clicker(&view); render(directory,"clicker-android-ready");
    assert(contains(lv_screen_active(),"安卓模式 · 触控滑动"));
    assert(contains(lv_screen_active(),"上键：上一条"));
    assert(contains(lv_screen_active(),"下键：下一条"));
    assert(ui_plays_clicker_key(UI_PLAY_UP)==UI_CLICK_ACTION_PREV);
    assert(ui_plays_clicker_key(UI_PLAY_DOWN)==UI_CLICK_ACTION_NEXT);
    ui_plays_clicker_key(UI_PLAY_OK); render(directory,"clicker-android-actions");
    assert(!contains(lv_screen_active(),"辅助触控"));
    assert(!contains(lv_screen_active(),"反转滚动方向"));
    ui_plays_close(); ui_plays_open(NULL, NULL, NULL); ui_plays_clicker(&view);
    ui_plays_clicker_key(UI_PLAY_UP); // The visible return entry wraps from first.
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_EXIT);
    ui_plays_close(); assert(lv_mem_test() == LV_RESULT_OK);
    puts("clicker UI: pairing help, state transitions, bounds and stable memory PASS");
}
