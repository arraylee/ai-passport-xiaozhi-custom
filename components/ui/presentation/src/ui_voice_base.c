// The dedicated XiaoZhi build has no name-card homepage. Keep only the shared
// configuration ABI: legacy metadata notifications must not create hidden UI,
// timers, avatar buffers, navigation targets or a second battery display.
#include "presentation/ui_profile.h"
#include "presentation/ui_theme.h"
#include "lvgl.h"

void ui_profile_create(void) {
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}
bool ui_profile_is_active(void) { return false; }
bool ui_profile_open_licenses(void) { return false; }
const void *ui_font_cn16(void) { return &ui_font_body; }
const void *ui_font_cn24(void) { return &ui_font_title; }
void ui_profile_set_name(const char *value) { (void)value; }
void ui_profile_set_title(const char *value) { (void)value; }
void ui_profile_set_intro(const char *value) { (void)value; }
void ui_profile_set_ble_connected(bool connected) { (void)connected; }
void ui_profile_set_battery(int percent, int level) { (void)percent; (void)level; }
void ui_profile_show_image(const void *image) { (void)image; }
void ui_profile_set_on_action(ui_action_cb_t callback, void *user) {
    (void)callback;
    (void)user;
}
