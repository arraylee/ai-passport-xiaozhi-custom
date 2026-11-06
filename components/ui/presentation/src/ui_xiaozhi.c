#include "presentation/ui_xiaozhi.h"
#include "presentation/ui_theme.h"
#include "presentation/ui_xiaozhi_theme.h"
#include <stdio.h>
#include <string.h>
#include "xiaozhi_volume_digits.inc"
static struct {
  lv_obj_t *root, *previous, *portrait, *message, *badge;
  lv_obj_t *choice[5], *caption[5], *icon[5], *volume_value, *volume_bar;
  lv_timer_t *motion;
  ui_battery_icon_t battery;
  xz_view_t view;
  const companion_portrait_t *art;
  unsigned page, selection, frame, blink_cycle;
  unsigned edit_volume;
  uint32_t expression_at, blink_started, blink_delay;
  bool greeting, setting_error, welcome_drawn;
} X;
static void welcome_drawn(lv_event_t *event) { (void)event; X.welcome_drawn = true; }
bool ui_xiaozhi_welcome_drawn(void) { return X.welcome_drawn; }
static void screen_loaded(lv_event_t *event) {
  lv_obj_t *screen = lv_event_get_target_obj(event);
  // The board has already set the rounded background. Every item on this
  // screen fits inside its 30px corners, so compositing the entire subtree
  // into an intermediate ARGB layer is unnecessary on this small heap.
  if (lv_obj_get_style_radius(screen, LV_PART_MAIN) <= 30)
    lv_obj_set_style_clip_corner(screen, false, LV_PART_MAIN);
}
static int battery_percent = -1, battery_level;
void ui_xiaozhi_set_battery(int percent, int level) {
  battery_percent = percent; battery_level = level;
  if (X.root) ui_theme_battery_set(&X.battery, percent, level);
}
enum { CHAT, APPEARANCE, MENU, HELP, INTERRUPT_MODE, VOLUME };
enum { MENU_WIFI, MENU_VOLUME, MENU_INTERRUPT, MENU_AGENT, MENU_RETURN, MENU_COUNT };
static const char *phase_name(xz_phase_t p) {
  switch (p) {
  case XZ_BOOT: case XZ_WELCOME: case XZ_SETUP: case XZ_LISTEN:
    return "";
  case XZ_WIFI:
    return "正在连接 Wi-Fi";
  case XZ_CLOUD:
    return "联网成功";
  case XZ_BIND:
    return "绑定设备，配置角色";
  case XZ_FAREWELL:
    return "拜拜";
  case XZ_WAKING:
    return "正在连接";
  case XZ_STANDBY:
    return "在线待机";
  case XZ_READY:
    return "在你身边";
  case XZ_THINK:
    return "想一想";
  case XZ_SPEAK:
    return "和你说话";
  case XZ_ERROR:
    if (X.view.restart_required || X.view.stage == XZ_STAGE_DEVICE) return "设备暂不可用";
    switch (X.view.stage) {
    case XZ_STAGE_WIFI: return "Wi-Fi 未连接";
    case XZ_STAGE_CLOUD: return "小智服务未连接";
    case XZ_STAGE_BIND: return "设备绑定未完成";
    case XZ_STAGE_VOICE: return "语音连接未完成";
    default: return "连接未完成";
    }
  case XZ_OFF:
    return "对话已结束";
  default:
    return "正在连接";
  }
}
static unsigned chat_control_count(xz_phase_t phase) {
  if (phase == XZ_WELCOME || phase == XZ_BOOT) return 1;
  return phase == XZ_FAREWELL || phase == XZ_WAKING || phase == XZ_STANDBY || phase == XZ_READY || phase == XZ_LISTEN ||
         phase == XZ_THINK || phase == XZ_SPEAK ? 3 : 2;
}
static lv_obj_t *text(const char *s, int x, int y, int w, const lv_font_t *font,
                      uint32_t color) {
  return ui_theme_text(X.root, s, x, y, w, font, color);
}
static void center(lv_obj_t *o) {
  lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
}
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, bool focused) {
  lv_obj_t *o = ui_theme_panel(parent, x, y, w, h, false);
  lv_obj_set_style_radius(o, 12, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(focused ? XZ_UI_ACCENT : XZ_UI_PANEL), 0);
  lv_obj_set_style_border_color(o, lv_color_hex(focused ? XZ_UI_ACCENT : XZ_UI_LINE), 0);
  return o;
}
static void footer(const char *hint) {
  center(text(hint, 16, 292, 208, &ui_font_caption, XZ_UI_MUTED));
}
static void message_text(const char *detail) {
  lv_point_t size;
  lv_text_get_size(&size, detail, &ui_font_body, 0, 3, 196, LV_TEXT_FLAG_NONE);
  int y = size.y <= ui_font_body.line_height ? (54 - ui_font_body.line_height) / 2 : 6;
  lv_label_set_text(X.message, detail);
  lv_obj_set_pos(X.message, 10, y);
  lv_obj_set_height(X.message, 54 - 2 * y);
}
static const char *primary_action(xz_phase_t phase) {
  if (phase == XZ_STANDBY) return "对话";
  if (phase == XZ_SPEAK || phase == XZ_THINK) return "打断";
  return "结束";
}
static void set_focus(unsigned index, bool selected) {
  if (index >= 5 || !X.choice[index]) return;
  lv_obj_set_style_bg_color(X.choice[index], lv_color_hex(selected ? XZ_UI_ACCENT : XZ_UI_PANEL), 0);
  lv_obj_set_style_border_color(X.choice[index], lv_color_hex(selected ? XZ_UI_ACCENT : XZ_UI_LINE), 0);
  lv_obj_set_style_text_color(X.caption[index], lv_color_hex(selected ? XZ_UI_ON_ACCENT : XZ_UI_INK), 0);
  if (X.icon[index])
    lv_obj_set_style_text_color(X.icon[index], lv_color_hex(selected ? XZ_UI_ON_ACCENT : XZ_UI_ACCENT), 0);
}
static void select_item(unsigned next) {
  if (X.selection == next) return;
  set_focus(X.selection, false);
  X.selection = next;
  set_focus(next, true);
}
static void control(const char *s, unsigned i, unsigned count, int y) {
  int width = (216 - (count - 1) * 6) / (int)count;
  lv_obj_t *box = panel(X.root, 12 + i * (width + 6), y, width, 32,
                                 X.selection == i);
  lv_obj_t *l = ui_theme_text(box, s, 2, 5, width - 4, &ui_font_title,
                              X.selection == i ? XZ_UI_ON_ACCENT : XZ_UI_INK);
  center(l);
  X.choice[i] = box; X.caption[i] = l;
  if (count == 1 && !strcmp(s, "配置网络"))
    X.icon[i] = ui_theme_text(box, LV_SYMBOL_WIFI, 35, 9, 22, &lv_font_montserrat_14,
                             X.selection == i ? XZ_UI_ON_ACCENT : XZ_UI_ACCENT);
}
static void motion(lv_timer_t *timer) {
  if (!X.root || !X.portrait || !X.art || !X.art->frames) {
    if (timer) lv_timer_pause(timer);
    return;
  }
  uint32_t age = lv_tick_elaps(X.expression_at);
  uint32_t blink = lv_tick_elaps(X.blink_started);
  if (blink >= X.blink_delay + 140) {
    X.blink_started = lv_tick_get(); blink = 0;
    X.blink_delay = 3400 + (++X.blink_cycle * 791 + X.view.avatar * 421) % 2000;
  }
  unsigned frame = COMPANION_IDLE;
  bool speaking = X.page == CHAT && X.view.output_active &&
      (X.view.phase == XZ_SPEAK || X.view.phase == XZ_FAREWELL || X.view.phase == XZ_WELCOME);
  uint32_t next = blink < X.blink_delay ? X.blink_delay - blink : X.blink_delay + 140 - blink;
  if (X.page == CHAT) {
    if (X.view.phase == XZ_LISTEN) frame = COMPANION_LISTEN;
    else if (X.view.phase == XZ_THINK) frame = COMPANION_THINK;
    else if (speaking) {
      frame = (age / 180) % 2 ? COMPANION_IDLE : COMPANION_TALK;
      if (180 - age % 180 < next) next = 180 - age % 180;
    } else if (X.view.phase == XZ_READY && X.greeting && age < 650) {
      frame = COMPANION_HAPPY;
      if (650 - age < next) next = 650 - age;
    }
  }
  if (blink >= X.blink_delay) frame = speaking ? COMPANION_HAPPY : COMPANION_BLINK;
  if (frame != X.frame) {
    lv_image_set_src(X.portrait, &X.art->frames[frame]);
    X.frame = frame;
  }
  // Idle wakes only for the next blink, not at animation-frame frequency.
  if (timer) lv_timer_set_period(timer, next ? next : 1);
}
static void speaking_mouth(lv_event_t *event) {
  // Native geometry on the existing portrait: no extra bitmap, object or timer.
  // The male's old 13x9 mouth was difficult to read at 144px.
  if (X.view.avatar!=1 || !X.view.output_active || X.page!=CHAT ||
      (X.frame!=COMPANION_TALK && X.frame!=COMPANION_HAPPY) ||
      (X.view.phase!=XZ_SPEAK && X.view.phase!=XZ_FAREWELL && X.view.phase!=XZ_WELCOME)) return;
  lv_area_t face; lv_obj_get_coords(lv_event_get_target_obj(event),&face);
  lv_layer_t *layer=lv_event_get_layer(event);
  lv_draw_rect_dsc_t mouth; lv_draw_rect_dsc_init(&mouth);
  mouth.bg_opa=LV_OPA_COVER; mouth.bg_color=lv_color_hex(0x56251F);
  mouth.radius=LV_RADIUS_CIRCLE; mouth.border_width=1;
  mouth.border_color=lv_color_hex(0x8C4232); mouth.border_opa=LV_OPA_COVER;
  lv_area_t open={face.x1+64,face.y1+107,face.x1+84,face.y1+123};
  lv_draw_rect(layer,&mouth,&open);
  mouth.bg_color=lv_color_hex(0xDE785E); mouth.border_width=0; mouth.radius=4;
  lv_area_t tongue={face.x1+68,face.y1+117,face.x1+80,face.y1+122};
  lv_draw_rect(layer,&mouth,&tongue);
}
static void show_portrait(int y) {
  if (!X.art || !X.art->frames) {
    center(text("形象资源未就绪", 20, y + 64, 200, &ui_font_title, XZ_UI_INK));
    return;
  }
  X.portrait = lv_image_create(X.root);
  lv_obj_add_event_cb(X.portrait,speaking_mouth,LV_EVENT_DRAW_POST,NULL);
  lv_obj_set_pos(X.portrait, 48, y);
  X.frame = COMPANION_FACE_COUNT;
  motion(X.motion);
  lv_timer_resume(X.motion);
}
static void render(void) {
  if (X.motion) lv_timer_pause(X.motion);
  X.welcome_drawn = false;
  lv_obj_clean(X.root);
  X.portrait = X.message = X.badge = X.volume_value = X.volume_bar = NULL;
  memset(X.choice, 0, sizeof X.choice); memset(X.caption, 0, sizeof X.caption);
  memset(X.icon, 0, sizeof X.icon);
  if (X.page == CHAT && X.view.phase == XZ_SETUP) {
    text("配置网络", 16, 14, 180, &ui_font_title, XZ_UI_INK);
  } else if (X.page == CHAT) {
    text("FoloToy", 16, 16, 72, &lv_font_montserrat_14, XZ_UI_ACCENT);
    text("小智", 93, 12, 70, &ui_font_title, XZ_UI_INK);
  } else text(X.page == APPEARANCE ? "换个形象" : X.page == MENU ? "小智设置" :
       X.page == INTERRUPT_MODE ? "打断方式" : X.page == VOLUME ? "调整音量" :
       "智能体设置", 16, 14, 180, &ui_font_title, XZ_UI_INK);
  ui_theme_battery_create(&X.battery, X.root);
  ui_theme_battery_set(&X.battery, battery_percent, battery_level);
  if (X.page == MENU) {
    static const char *const items[MENU_COUNT] = {
        "重新配网", "调整音量", "打断方式", "智能体设置", "返回"};
    static const char *const icons[MENU_COUNT] = {
        LV_SYMBOL_WIFI, LV_SYMBOL_VOLUME_MAX, LV_SYMBOL_PAUSE, LV_SYMBOL_SETTINGS, LV_SYMBOL_LEFT};
    for (unsigned i = 0; i < MENU_COUNT; i++) {
      lv_obj_t *row =
          panel(X.root, 14, 54 + 45 * i, 212, 38, X.selection == i);
      X.choice[i] = row;
      X.icon[i] = ui_theme_text(row, icons[i], 12, 11, 22, &lv_font_montserrat_14,
                    X.selection == i ? XZ_UI_ON_ACCENT : XZ_UI_ACCENT);
      X.caption[i] = ui_theme_text(row, items[i], 42, 8, 158, &ui_font_title,
                    X.selection == i ? XZ_UI_ON_ACCENT : XZ_UI_INK);
    }
    footer("上下选择   确定进入");
    return;
  }
  if (X.page == INTERRUPT_MODE) {
    const char *modes[] = {"按键打断", "自然打断（测试功能）", "返回菜单"};
    for (unsigned i = 0; i < 3; ++i) {
      lv_obj_t *row = panel(X.root, 14, 76 + 53 * i, 212, 43, X.selection == i);
      X.choice[i] = row;
      X.caption[i] = ui_theme_text(row, modes[i], 12, 10, 188,
                    i == 1 ? &ui_font_body : &ui_font_title, X.selection == i ? XZ_UI_ON_ACCENT : XZ_UI_INK);
    }
    if (X.setting_error) text("保存失败，请重试", 16, 244, 208, &ui_font_title, XZ_UI_ACCENT);
    else
      text("自然打断建议环境安静", 16, 244, 208, &ui_font_caption, XZ_UI_MUTED);
    footer("上下选择   确定选用");
    return;
  }
  if (X.page == VOLUME) {
    char volume[16]; snprintf(volume, sizeof volume, "%u", X.edit_volume);
    X.volume_value = text(volume, 16, 91, 208, &xz_volume_digits, XZ_UI_INK);
    center(X.volume_value);
    lv_obj_t *bar = X.volume_bar = lv_bar_create(X.root);
    lv_obj_set_pos(bar, 40, 170); lv_obj_set_size(bar, 160, 12);
    lv_obj_set_style_bg_color(bar, lv_color_hex(XZ_UI_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(XZ_UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 6, LV_PART_INDICATOR);
    text(LV_SYMBOL_VOLUME_MID, 16, 167, 20, &lv_font_montserrat_14, XZ_UI_ACCENT);
    text(LV_SYMBOL_VOLUME_MAX, 204, 167, 24, &lv_font_montserrat_14, XZ_UI_ACCENT);
    lv_bar_set_range(bar, 0, 100); lv_bar_set_value(bar, X.edit_volume, LV_ANIM_OFF);
    center(text("上下调整 · 确定保存", 12, 215, 216, &ui_font_title, XZ_UI_INK));
    if (X.setting_error) center(text("保存失败，请重试", 12, 249, 216, &ui_font_title, XZ_UI_ACCENT));
    footer("长按确定   取消");
    return;
  }
  if (X.page == HELP) {
    panel(X.root, 14, 104, 212, 86, false);
    center(text("前往xiaozhi.me，\n进入控制台设置", 16, 124, 208, &ui_font_title, XZ_UI_INK));
    control("返回", 0, 1, 274);
    return;
  }
  if (X.page == CHAT && (X.view.phase == XZ_BOOT || X.view.phase == XZ_WELCOME)) {
    show_portrait(49);
    if (X.view.phase == XZ_WELCOME) {
      center(text("hi，我是FoloToy的小智", 14, 205, 212, &ui_font_title, XZ_UI_INK));
      center(text("请按右侧最下方的按钮", 14, 237, 212, &ui_font_body, XZ_UI_MUTED));
      control("配置网络", 0, 1, 274);
      lv_obj_add_event_cb(X.choice[0], welcome_drawn, LV_EVENT_DRAW_POST, NULL);
    }
    return;
  }
  if (X.page == CHAT && (X.view.phase == XZ_SETUP || X.view.phase == XZ_BIND ||
                         X.view.phase == XZ_WIFI || X.view.phase == XZ_CLOUD ||
                         X.view.phase == XZ_ERROR || X.view.phase == XZ_OFF)) {
    if (X.view.phase == XZ_SETUP) {
      panel(X.root, 14, 94, 212, 78, false);
      panel(X.root, 14, 184, 212, 77, false);
      if (*X.view.detail) {
        X.message=text(X.view.detail,16,71,208,&ui_font_title,XZ_UI_ACCENT);
        lv_obj_set_height(X.message,22);
        lv_label_set_long_mode(X.message,LV_LABEL_LONG_DOT);
      }
      text("01", 25, 106, 24, &lv_font_montserrat_14, XZ_UI_ACCENT);
      text("手机连接下方热点",55,105,160,&ui_font_body,XZ_UI_INK);
      text(X.view.hotspot,25,137,190,&ui_font_body,XZ_UI_ACCENT);
      text("02", 25, 196, 24, &lv_font_montserrat_14, XZ_UI_ACCENT);
      text("打开 192.168.4.1",55,195,160,&ui_font_body,XZ_UI_INK);
      text("填写 2.4G Wi-Fi",55,227,160,&ui_font_body,XZ_UI_MUTED);
    } else if (X.view.phase == XZ_BIND) {
      text("联网成功", 16, 65, 208, &ui_font_title, XZ_UI_ACCENT);
      text("请前往 xiaozhi.me", 16, 101, 208, &ui_font_title, XZ_UI_ACCENT);
      text("登录并进入", 16, 136, 100, &ui_font_title, XZ_UI_INK);
      text("控制台", 116, 136, 108, &ui_font_title, XZ_UI_ACCENT);
      text("添加设备时输入", 16, 170, 208, &ui_font_title, XZ_UI_INK);
      center(text(X.view.code, 16, 207, 208, &lv_font_montserrat_28, XZ_UI_ACCENT));
      center(text("绑定", 16, 244, 208, &ui_font_title, XZ_UI_INK));
    } else {
      text(phase_name(X.view.phase), 16, 71, 208, &ui_font_title, XZ_UI_ACCENT);
      X.message = text(X.view.detail, 16, 111, 208, &ui_font_title, XZ_UI_INK);
      bool binding_code = X.view.stage == XZ_STAGE_BIND && X.view.code[0];
      lv_obj_set_height(X.message, binding_code ? 89 : 121);
      // Errors retain their complete text. Scroll overflow rather than replacing
      // the ending (often the actual cause/action) with an ellipsis.
      lv_label_set_long_mode(X.message, X.view.phase == XZ_ERROR ? LV_LABEL_LONG_SCROLL : LV_LABEL_LONG_DOT);
      if (X.view.phase == XZ_ERROR && binding_code)
        center(text(X.view.code, 16, 208, 208, &lv_font_montserrat_28, XZ_UI_ACCENT));
      if (X.view.wifi_saved)
        text("Wi-Fi 已保存", 16, 244, 208, &ui_font_title, XZ_UI_ACCENT);
    }
    const char *action = "请稍候";
    if (X.view.restart_required)
      action = "返回";
    else if (X.view.phase == XZ_BIND ||
             (X.view.phase == XZ_ERROR && X.view.stage == XZ_STAGE_BIND && X.view.code[0]))
      action = "重新获取";
    else if (X.view.phase == XZ_ERROR || X.view.phase == XZ_OFF)
      action = "重试";
    control(action, 0, 2, 274);
    control("菜单", 1, 2, 274);
    return;
  }
  char count[24];
  snprintf(count, sizeof count, "%02u / 02", X.view.avatar + 1);
  X.badge = text(X.page == APPEARANCE ? count : phase_name(X.view.phase), 14, 40, 212,
       &ui_font_title, XZ_UI_ACCENT);
  show_portrait(66);
  if (X.page == APPEARANCE) {
    center(text("上下切换 · 确定选用", 12, 236, 216, &ui_font_title, XZ_UI_INK));
    center(
        text("名字与性格在云端设置", 12, 274, 216, &ui_font_title, XZ_UI_INK));
    return;
  }
  lv_obj_t *bubble = panel(X.root, 12, 216, 216, 54, false);
  X.message =
      ui_theme_text(bubble, X.view.detail, 10, 6, 196, &ui_font_body, XZ_UI_INK);
  lv_obj_set_height(X.message, 42);
  lv_label_set_long_mode(X.message, LV_LABEL_LONG_DOT);
  center(X.message);
  message_text(X.view.detail);
  control(primary_action(X.view.phase), 0, 3, 276);
  control("形象", 1, 3, 276);
  control("菜单", 2, 3, 276);
}
void ui_xiaozhi_open(const xz_view_t *v, const companion_portrait_t *art) {
  if (X.root)
    return;
  memset(&X, 0, sizeof X);
  X.previous = lv_screen_active();
  X.root = ui_theme_screen();
  lv_obj_set_style_bg_color(X.root, lv_color_hex(XZ_UI_BG), 0);
  lv_obj_set_style_text_color(X.root, lv_color_hex(XZ_UI_INK), 0);
  lv_obj_add_event_cb(X.root, screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
  X.view = *v;
  X.art = art;
  X.expression_at = X.blink_started = lv_tick_get();
  X.blink_delay = 3400 + v->avatar * 421;
  X.motion = lv_timer_create(motion, 140, NULL);
  render();
  lv_screen_load(X.root);
}
void ui_xiaozhi_update(const xz_view_t *v, const companion_portrait_t *art) {
  if (!X.root)
    return;
  bool show_fault = v->restart_required && v->phase == XZ_ERROR && X.page != CHAT;
  if (show_fault) { X.page = CHAT; X.selection = 0; }
  bool phase_changed = v->phase != X.view.phase;
  bool audio_changed = v->output_active != X.view.output_active;
  bool art_changed = art != X.art || v->avatar != X.view.avatar;
  bool controls_changed = chat_control_count(v->phase) != chat_control_count(X.view.phase);
  bool detail_changed = strcmp(X.view.detail, v->detail) != 0;
  bool changed = show_fault || phase_changed || v->revision != X.view.revision || art_changed;
  if (phase_changed || art_changed) {
    X.greeting = X.view.phase == XZ_SPEAK && v->phase == XZ_READY;
    X.expression_at = X.blink_started = lv_tick_get();
    X.blink_delay = 3400 + v->avatar * 421;
  }
  X.view = *v;
  X.art = art;
  if (audio_changed && X.portrait && X.page == CHAT && !phase_changed && !art_changed) {
    if (v->output_active) X.expression_at = lv_tick_get();
    motion(X.motion); lv_timer_reset(X.motion);
  }
  if (!changed || (X.page != CHAT && X.page != APPEARANCE)) return;
  if (X.page == CHAT && controls_changed) X.selection = 0;
  // Keep the portrait, speech bubble and controls alive across speech updates.
  // Changing a sentence or moving focus must not invalidate a full LCD frame.
  if (!show_fault && X.badge && ((X.portrait != NULL) == (art && art->frames)) &&
      (X.page == APPEARANCE || chat_control_count(v->phase) == 3)) {
    if (X.page == CHAT) {
      if (phase_changed) {
        lv_label_set_text(X.badge, phase_name(v->phase));
        lv_label_set_text(X.caption[0], primary_action(v->phase));
      }
      if (detail_changed) message_text(v->detail);
    } else if (art_changed) {
      lv_label_set_text_fmt(X.badge, "%02u / 02", v->avatar + 1);
    }
    if (X.portrait && (art_changed || phase_changed)) {
      if (art_changed) X.frame = COMPANION_FACE_COUNT;
      motion(X.motion);
      lv_timer_reset(X.motion);
    }
    return;
  }
  render();
}
void ui_xiaozhi_setting_result(bool saved) {
  if (!X.root) return;
  X.setting_error = !saved;
  if (X.page == VOLUME || X.page == INTERRUPT_MODE) {
    if (saved) {
      X.selection = X.page == VOLUME ? MENU_VOLUME : MENU_INTERRUPT;
      X.page = MENU;
    }
    render();
  }
}
ui_xz_action_t ui_xiaozhi_key(ui_play_key_t key, unsigned *avatar) {
  if (!X.root)
    return UI_XZ_NONE;
  if (X.page == CHAT && (X.view.phase == XZ_BOOT || X.view.phase == XZ_WELCOME))
    return X.view.phase == XZ_WELCOME && key == UI_PLAY_OK ? UI_XZ_BEGIN_SETUP : UI_XZ_NONE;
  if (key == UI_PLAY_BACK) {
    if (X.page == CHAT && X.view.restart_required) return UI_XZ_EXIT;
    unsigned previous = X.page;
    X.page = previous == VOLUME || previous == INTERRUPT_MODE || previous == HELP ||
             previous == CHAT ? MENU : CHAT;
    X.selection = previous == VOLUME ? MENU_VOLUME : previous == INTERRUPT_MODE ? MENU_INTERRUPT :
                  previous == HELP ? MENU_AGENT : previous == APPEARANCE ? 1 : 0;
    X.setting_error = false;
    render();
    return UI_XZ_NONE;
  }
  if (X.page == VOLUME) {
    unsigned before = X.edit_volume;
    if (key == UI_PLAY_UP) X.edit_volume = X.edit_volume >= 95 ? 100 : X.edit_volume + 5;
    else if (key == UI_PLAY_DOWN) X.edit_volume = X.edit_volume <= 5 ? 0 : X.edit_volume - 5;
    else if (key == UI_PLAY_OK) {
      *avatar = X.edit_volume;
      X.setting_error = false;
      return UI_XZ_VOLUME;
    }
    if (X.edit_volume != before) {
      lv_label_set_text_fmt(X.volume_value, "%u", X.edit_volume);
      lv_bar_set_value(X.volume_bar, X.edit_volume, LV_ANIM_OFF);
    }
    return UI_XZ_NONE;
  }
  if (X.page == INTERRUPT_MODE) {
    if (key == UI_PLAY_UP || key == UI_PLAY_DOWN) {
      select_item((X.selection + (key == UI_PLAY_UP ? 2 : 1)) % 3);
      return UI_XZ_NONE;
    }
    else if (key == UI_PLAY_OK) {
      unsigned selected = X.selection;
      X.setting_error = false;
      if (selected < 2 && (selected == 1) != X.view.natural_interrupt) {
        *avatar = selected;
        return UI_XZ_INTERRUPT_MODE;
      }
      X.page = MENU; X.selection = MENU_INTERRUPT;
    }
    render(); return UI_XZ_NONE;
  }
  if (X.page == APPEARANCE) {
    if (key == UI_PLAY_UP || key == UI_PLAY_DOWN) {
      *avatar = (X.view.avatar + 1) % 2;
      return UI_XZ_AVATAR;
    }
    if (key == UI_PLAY_OK) {
      X.page = CHAT;
      X.selection = 1;
      render();
    }
    return UI_XZ_NONE;
  }
  if (X.page == HELP) {
    if (key == UI_PLAY_OK) {
      X.page = MENU;
      X.selection = MENU_AGENT;
      render();
    }
    return UI_XZ_NONE;
  }
  unsigned count = X.page == MENU ? MENU_COUNT : chat_control_count(X.view.phase);
  if (key == UI_PLAY_UP || key == UI_PLAY_DOWN) {
    select_item((X.selection + (key == UI_PLAY_UP ? count - 1 : 1)) % count);
    return UI_XZ_NONE;
  }
  else if (key == UI_PLAY_OK) {
    if (X.page == MENU) {
      unsigned s = X.selection;
      X.selection = 0;
      X.setting_error = false;
      X.page = s == MENU_VOLUME ? VOLUME : s == MENU_INTERRUPT ? INTERRUPT_MODE :
               s == MENU_AGENT ? HELP : CHAT;
      if (s == MENU_VOLUME) X.edit_volume = X.view.volume <= 100 ? X.view.volume : 100;
      if (s == MENU_INTERRUPT) X.selection = X.view.natural_interrupt ? 1 : 0;
      render();
      return s == MENU_WIFI ? UI_XZ_WIFI : UI_XZ_NONE;
    }
    if (X.selection == count - 1) {
      X.page = MENU;
      X.selection = 0;
    } else if (X.selection == 1) {
      X.page = APPEARANCE;
    } else if (X.view.phase == XZ_BIND)
      return UI_XZ_RETRY;
    else if (X.view.phase == XZ_ERROR || X.view.phase == XZ_OFF)
      return X.view.restart_required ? UI_XZ_EXIT : UI_XZ_RETRY;
    else if (X.view.phase == XZ_STANDBY) return UI_XZ_RESUME;
    else if (count == 3)
      return X.view.phase == XZ_SPEAK || X.view.phase == XZ_THINK ? UI_XZ_INTERRUPT : UI_XZ_TALK;
  }
  render();
  return UI_XZ_NONE;
}
void ui_xiaozhi_close(void) {
  if (!X.root)
    return;
  if (X.motion)
    lv_timer_delete(X.motion);
  lv_screen_load(X.previous);
  lv_obj_delete(X.root);
  memset(&X, 0, sizeof X);
}
