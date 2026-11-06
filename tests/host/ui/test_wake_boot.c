#include "lvgl.h"
#include "presentation/ui_profile.h"
#include "presentation/bobo_art.h"
#include "presentation/ui_button_guide.h"
#include "presentation/ui_selftest.h"
#include "platform_display_native.h"
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PERIPH_BLE 1
#define BRIGHT_PCT 100
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define JPEG_SLOT_AVATAR 1
#define JPEG_VIEW_AVATAR 1
typedef int esp_err_t;
typedef struct { int unused; } hal_display_t;
static hal_display_t fake_display, *s_disp = &fake_display;
static uint16_t buffer[240 * 20], avatar_pixels[96 * 156];
static const lv_image_dsc_t avatar = {.header = {.magic=LV_IMAGE_HEADER_MAGIC,
    .cf=LV_COLOR_FORMAT_RGB565, .w=96, .h=156, .stride=192},
    .data_size=sizeof avatar_pixels, .data=(const uint8_t *)avatar_pixels};
static unsigned lights, panels, flushes, image_requests;
static bool flushed_avatar, flushed_bear, lit_avatar, lit_bear;
static bool uploaded_exists, named_exists, factory_mode;
static atomic_bool s_config_connected;
static bool s_button_guide_active, s_button_guide_exit, guide_test;
static unsigned guide_waits;
static int *s_id, *s_button;
static void platform_lvgl_nav_enable(bool enabled) { (void)enabled; }
static bool platform_button_any_pressed(void *button) { (void)button; return false; }
#define HAL_IDENTITY_SN_CAP 13
#define HAL_IDENTITY_KEY_CAP 65
static int hal_identity_get_sn(void *id, char *out, size_t n) { (void)id; return snprintf(out,n,"PREVIEW15"); }
static int hal_identity_get_key(void *id, char *out, size_t n) { (void)id; return snprintf(out,n,"PREVIEW_ONLY"); }
static struct { char avatar_name[16]; } s_profile;
static bool platform_lvgl_lock(int timeout) { (void)timeout; return true; }
static void platform_lvgl_unlock(void) {}
static void note_activity(void) {}
static void vTaskDelay(int ticks);
static void on_dock_action(int action, void *user) { (void)action; (void)user; }
static void *platform_display_panel(hal_display_t *display) { assert(display == s_disp); return display; }
static const char *esp_err_to_name(int error) { (void)error; return "fake"; }
static int esp_lcd_panel_disp_on_off(void *panel, bool on) {
    assert(panel == s_disp && on && flushes); ++panels; return ESP_OK;
}
static int hal_display_set_backlight(hal_display_t *display, uint8_t brightness) {
    assert(display == s_disp && brightness == 100 && panels == lights + 1);
    ++lights; lit_avatar = flushed_avatar; lit_bear = flushed_bear; return 0;
}
static bool jpeg_store_has_valid(int slot) { assert(slot == JPEG_SLOT_AVATAR); return uploaded_exists; }
static bool avatar_store_has(const char *name) { assert(name == s_profile.avatar_name); return named_exists; }
static void jpeg_view_request_mode(int mode) { assert(mode == JPEG_VIEW_AVATAR); ++image_requests; }
static void jpeg_view_request_avatar(const char *name) { assert(name == s_profile.avatar_name); ++image_requests; }
#include "wake_boot_production.inc"
static void vTaskDelay(int ticks) {
    (void)ticks;
    if (!guide_test) return;
    ++guide_waits;
    if (guide_waits == 1) {
        s_button_guide_exit = true; // Skip before decoding completes.
        assert(!atomic_load(&s_boot_avatar_ready));
    } else if (guide_waits == 2) {
        assert(s_button_guide_active); // QR must still cover unfinished home.
        on_jpeg_avatar(&avatar, NULL);
    }
    lv_refr_now(NULL);
}

static bool contains_image(lv_obj_t *object, const void *source) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return false;
    if (lv_obj_check_type(object, &lv_image_class) && lv_image_get_src(object) == source) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (contains_image(lv_obj_get_child(object, i), source)) return true;
    return false;
}
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; ++flushes;
    flushed_avatar = contains_image(lv_screen_active(), &avatar);
    flushed_bear = contains_image(lv_screen_active(), &bobo_art_wave);
    lv_display_flush_ready(display);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    lv_init(); lv_display_t *display = native_platform_display(flush);
    factory_mode = !strcmp(argv[1], "factory");
    guide_test = !strcmp(argv[1], "qr");
    bool cold = !strncmp(argv[1], "cold", 4) || factory_mode || guide_test;
    boot_profile(!cold, display);
    lv_refr_now(NULL);
    if (factory_mode) {
        assert(lights == 1);
        /* Execute the actual boot block before rendering the real self-test.
         * Retaining an unused home here leaves insufficient layer workspace. */
        ui_selftest_open("PREVIEW01");
        const char *items[] = {"INFO", "I2C", "AUDIO", "BATT", "BLE", "ID", "DISP", "BTN"};
        for (unsigned i = 0; i < 8; ++i) ui_selftest_set_item(items[i], UI_ST_PASS);
        ui_selftest_battery(70);
        ui_selftest_prompt("各按两下: 上 0/2  下 0/2  确定 0/2");
        lv_refr_now(NULL);
        ui_selftest_result(true); lv_refr_now(NULL);
        ui_selftest_result(false); lv_refr_now(NULL);
        assert(lv_mem_test() == LV_RESULT_OK && lights == 1);
        lv_mem_monitor_t memory; lv_mem_monitor(&memory);
        printf("Factory rounded progress/pass/fail render PASS peak=%zu largest=%zu\n",
               memory.max_used, memory.free_biggest_size);
        return 0;
    }
    if (guide_test) {
        strcpy(s_profile.avatar_name, "avatar-03"); named_exists = true;
        show_boot_button_guide();
        lv_refr_now(NULL);
        assert(image_requests == 1 && guide_waits >= 2 && lights == 1);
        assert(atomic_load(&s_boot_avatar_ready) && !s_button_guide_active);
        assert(flushed_avatar && !flushed_bear && ui_profile_is_active());
        puts("QR skip waits for avatar and exposes one complete home PASS"); return 0;
    }
    if (cold && lights) { fputs("FAIL: cold boot lit the Bobo placeholder before saved-avatar restore\n", stderr); return 1; }
    if (lights) { fputs("FAIL: wake lit the Bobo placeholder before saved-avatar restore\n", stderr); return 1; }
    // Slow decode must not expose an interim bear after a fixed short delay.
    for (unsigned i = 0; i < 100; ++i) { lv_tick_inc(20); lv_timer_handler(); }
    assert(lights == 0);
    bool missing = strstr(argv[1], "missing") != NULL, failed = strstr(argv[1], "failed") != NULL;
#if HAS_BOOT_RESTORE
    if (!missing) {
        if (strstr(argv[1], "uploaded") != NULL) uploaded_exists = true;
        else { strcpy(s_profile.avatar_name, "avatar-03"); named_exists = true; }
    }
    restore_boot_avatar();
    assert(image_requests == (missing ? 0 : 1));
#endif
    if (!missing) on_jpeg_avatar(failed ? NULL : &avatar, NULL);
    assert(lights == 0); // Installing an image alone is not a completed LCD frame.
    lv_refr_now(NULL);
    assert(lights == 1 && panels == 1);
    assert(lit_avatar == !(failed || missing));
    assert(lit_bear == (failed || missing));
    on_jpeg_avatar(&avatar, NULL); lv_refr_now(NULL);
    assert(lights == 1); // Later avatar updates cannot re-arm the wake gate.
    assert(lv_mem_test() == LV_RESULT_OK);
    printf("Wake %s: correct first visible frame, no premature light, one enable PASS\n", argv[1]);
}
