#include "presentation/ui_boot.h"
#include "lvgl.h"
#include <string.h>
#include "boot_logo.inc"
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif
static struct {
  lv_obj_t *root, *parts[2];
  lv_timer_t *timer;
  uint32_t started, last_tick;
  uint16_t ticks, max_gap;
  bool drawn;
} B;
// Anticipation, fast latch, shared impact, then one return to rest.
// Coordinates are native pixels; no transformed layer or full-screen buffer.
static const struct { uint16_t ms; int8_t left, right; } poses[] = {
  {0,0,-22}, {267,0,-24}, {334,0,-24}, {367,0,-22},
  {400,0,-17}, {434,0,-10}, {467,0,-3}, {500,4,4},
  {534,3,3}, {567,2,2}, {601,2,2}, {634,1,1},
  {667,1,1}, {701,0,0}, {734,0,0}
};
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
    if (gap>B.max_gap) B.max_gap=(uint16_t)(gap>UINT16_MAX?UINT16_MAX:gap);
    B.last_tick=lv_tick_get();
    B.ticks++;
  }
  unsigned i=0, last=sizeof poses/sizeof poses[0]-1;
  while (i<last && age>=poses[i+1].ms) i++;
  int left=poses[i].left, right=poses[i].right;
  if (i<last) {
    int elapsed=(int)age-poses[i].ms, duration=poses[i+1].ms-poses[i].ms;
    left+=(poses[i+1].left-left)*elapsed/duration;
    right+=(poses[i+1].right-right)*elapsed/duration;
  }
  int x = (240-BOOT_LOGO_WIDTH)/2, y = (320-BOOT_LOGO_HEIGHT)/2;
  lv_obj_set_pos(B.parts[0],x,y+left);
  lv_obj_set_pos(B.parts[1],x+BOOT_LOGO_SPLIT,y+right);
  if (age>=poses[last].ms) {
    lv_timer_pause(B.timer);
#ifdef ESP_PLATFORM
    ESP_LOGI("boot_logo","animation_done ticks=%u max_tick_gap_ms=%u",(unsigned)B.ticks,(unsigned)B.max_gap);
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
  for (unsigned i=0;i<2;i++) {
    B.parts[i]=lv_image_create(B.root);
    lv_image_set_src(B.parts[i],&boot_logo[i]);
    lv_obj_set_style_image_recolor(B.parts[i],lv_color_hex(BOOT_LOGO_COLOR),0);
    lv_obj_set_style_image_recolor_opa(B.parts[i],LV_OPA_COVER,0);
  }
  B.timer=lv_timer_create(tick,33,NULL);
  if (!B.timer) { ui_boot_close(); return false; }
  tick(B.timer);
  return true;
}
bool ui_boot_drawn(void) { return B.root && B.drawn; }
void ui_boot_close(void) {
  if (B.timer) lv_timer_delete(B.timer);
  if (B.root) lv_obj_delete(B.root);
  memset(&B,0,sizeof B);
}
