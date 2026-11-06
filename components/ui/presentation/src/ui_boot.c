#include "presentation/ui_boot.h"
#include "lvgl.h"
#include <string.h>
#include "boot_logo.inc"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif
static struct {
  lv_obj_t *root, *logo, *covered;
  lv_timer_t *timer;
  uint32_t started, last_tick, slow_at;
  uint16_t ticks, max_gap;
  lv_opa_t covered_opa;
  bool drawn, covered_had_opa;
} B;
#define LOGO_SCROLL_MS 2600u
static void drawn(lv_event_t *event) {
  (void)event;
  if (!B.drawn) {
    B.drawn=true;
    B.started=B.last_tick=lv_tick_get();
  }
}
static void tick(lv_timer_t *timer) {
  (void)timer;
  unsigned age=B.drawn?lv_tick_elaps(B.started):0;
  if (B.drawn) {
    uint32_t gap=lv_tick_elaps(B.last_tick);
    if (gap>B.max_gap) {
      B.max_gap=(uint16_t)(gap>UINT16_MAX?UINT16_MAX:gap);
      B.slow_at=age;
    }
    B.last_tick=lv_tick_get();
    B.ticks++;
  }
  // One intact wordmark scrolls at constant speed, then rests at the center.
  int x = (240-BOOT_LOGO_WIDTH)/2, y = (320-BOOT_LOGO_HEIGHT)/2;
  unsigned elapsed=age<LOGO_SCROLL_MS?age:LOGO_SCROLL_MS;
  int top=-BOOT_LOGO_HEIGHT+(int)((y+BOOT_LOGO_HEIGHT)*elapsed/LOGO_SCROLL_MS);
  lv_obj_set_pos(B.logo,x,top);
  if (age>=LOGO_SCROLL_MS) {
    lv_timer_pause(B.timer);
#ifdef ESP_PLATFORM
    ESP_LOGI("boot_logo","animation_done ticks=%u max_tick_gap_ms=%u slow_at_ms=%u",(unsigned)B.ticks,(unsigned)B.max_gap,(unsigned)B.slow_at);
#endif
  }
}
bool ui_boot_open(void) {
  if (B.root) return true;
  memset(&B,0,sizeof B);
  B.root=lv_obj_create(lv_layer_top());
  if (!B.root) return false;
  lv_obj_remove_style_all(B.root);
  lv_obj_set_size(B.root,240,320);
  lv_obj_set_style_bg_color(B.root,lv_color_hex(0x000000),0);
  lv_obj_set_style_bg_opa(B.root,LV_OPA_COVER,0);
  lv_obj_remove_flag(B.root,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(B.root,drawn,LV_EVENT_DRAW_POST,NULL);
  B.logo=lv_image_create(B.root);
  lv_image_set_src(B.logo,&boot_logo);
  lv_obj_set_style_image_recolor(B.logo,lv_color_hex(BOOT_LOGO_COLOR),0);
  lv_obj_set_style_image_recolor_opa(B.logo,LV_OPA_COVER,0);
  // LVGL draws the active screen before the top layer even if it is covered.
  // Avoid repeatedly decoding/rendering the hidden legacy home under the logo.
  // OPA_LAYERED == 0 skips the entire subtree before any layer allocation.
  // Do not use HIDDEN on a screen: removing it touches a nonexistent parent.
  B.covered=lv_screen_active();
  lv_style_value_t previous;
  B.covered_had_opa=lv_obj_get_local_style_prop(B.covered,LV_STYLE_OPA_LAYERED,&previous,0)==LV_STYLE_RES_FOUND;
  if (B.covered_had_opa) B.covered_opa=(lv_opa_t)previous.num;
  lv_obj_set_style_opa_layered(B.covered,LV_OPA_TRANSP,0);
  B.timer=lv_timer_create(tick,33,NULL);
  if (!B.timer) { ui_boot_close(); return false; }
  tick(B.timer);
  return true;
}
bool ui_boot_drawn(void) { return B.root && B.drawn; }
void ui_boot_close(void) {
  if (B.timer) lv_timer_delete(B.timer);
  if (B.root) lv_obj_delete(B.root);
  if (B.covered && lv_obj_is_valid(B.covered)) {
    if (B.covered_had_opa) lv_obj_set_style_opa_layered(B.covered,B.covered_opa,0);
    else lv_obj_remove_local_style_prop(B.covered,LV_STYLE_OPA_LAYERED,0);
  }
  memset(&B,0,sizeof B);
}
