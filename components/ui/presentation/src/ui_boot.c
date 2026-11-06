#include "presentation/ui_boot.h"
#include "lvgl.h"
#include <string.h>
#include "boot_logo.inc"
static struct {
  lv_obj_t *root, *parts[2];
  lv_timer_t *timer;
  uint32_t started;
  bool drawn;
} B;
static unsigned ramp(unsigned age, unsigned start, unsigned duration) {
  return age <= start ? 0 : age >= start + duration ? 255 : (age-start)*255/duration;
}
static void drawn(lv_event_t *event) { (void)event; B.drawn = true; }
static void tick(lv_timer_t *timer) {
  (void)timer;
  unsigned age = lv_tick_elaps(B.started);
  int p = (int)ramp(age, 170, 190);
  // The right side drops into place, then settles with a small click-like snap.
  // Integer translation only: no scaling, transformed layers or frame buffers.
  int drop = age < 360 ? -(28*(255*255-p*p))/(255*255) :
             age < 410 ? 2 : age < 460 ? -1 : 0;
  int x = (240-BOOT_LOGO_WIDTH)/2, y = (320-BOOT_LOGO_HEIGHT)/2;
  lv_obj_set_pos(B.parts[0],x,y);
  lv_obj_set_pos(B.parts[1],x+BOOT_LOGO_SPLIT,y+drop);
  for (unsigned i=0;i<2;i++)
    lv_obj_set_style_image_opa(B.parts[i],ramp(age,40,90),0);
  if (age >= 500) lv_timer_pause(B.timer);
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
  B.started=lv_tick_get();
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
