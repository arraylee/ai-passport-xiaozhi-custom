#include "presentation/ui_boot.h"
#include "presentation/ui_theme.h"
#include "lvgl.h"
#include <string.h>
#include "boot_logo.inc"
static struct {
  lv_obj_t *root, *parts[4], *name, *edition;
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
  unsigned age = lv_tick_elaps(B.started), p = ramp(age, 0, 360);
  unsigned remaining = 255-p;
  int distance = (20*remaining*remaining)/(255*255);
  int bounce = age >= 360 && age < 480 ? -2 : age >= 480 && age < 580 ? 1 : 0;
  for (unsigned i=0;i<4;i++) {
    int side = i % 2;
    lv_obj_set_pos(B.parts[i],64+side*56+(side?distance:-distance),76+bounce);
    lv_obj_set_style_image_opa(B.parts[i],ramp(age,0,200),0);
  }
  lv_obj_set_style_text_opa(B.name,ramp(age,320,260),0);
  lv_obj_set_style_text_opa(B.edition,ramp(age,560,280),0);
  if (age >= 900) lv_timer_pause(B.timer);
}
bool ui_boot_open(void) {
  if (B.root) return true;
  memset(&B,0,sizeof B);
  // An opaque top-layer rectangle occludes the old page. No full-screen alpha
  // composition, transformed image or intermediate 240x320 animation buffer.
  B.root=lv_obj_create(lv_layer_top());
  if (!B.root) return false;
  lv_obj_remove_style_all(B.root);
  lv_obj_set_size(B.root,240,320);
  lv_obj_set_style_bg_color(B.root,lv_color_hex(0x050709),0);
  lv_obj_set_style_bg_opa(B.root,LV_OPA_COVER,0);
  lv_obj_remove_flag(B.root,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(B.root,drawn,LV_EVENT_DRAW_POST,NULL);
  for (unsigned i=0;i<4;i++) {
    B.parts[i]=lv_image_create(B.root);
    lv_image_set_src(B.parts[i],&boot_logo[i]);
    lv_obj_set_style_image_recolor(B.parts[i],lv_color_hex(i<2?0xDB2777:0xFFFFFF),0);
    lv_obj_set_style_image_recolor_opa(B.parts[i],LV_OPA_COVER,0);
  }
  B.name=ui_theme_text(B.root,"FoloToy",14,209,212,&lv_font_montserrat_28,0xF5F3EF);
  B.edition=ui_theme_text(B.root,"AI PASSPORT",14,253,212,&lv_font_montserrat_12,0xB0ABA7);
  lv_obj_set_style_text_align(B.name,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_text_align(B.edition,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_style_text_letter_space(B.edition,3,0);
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
