// components/presentation/include/presentation/ui_profile.h
#pragma once
#include <stdbool.h>
#include <stdint.h>

// 构建主页(头像 + 昵称/头衔/四行简介 + 玩法/相册 dock)。
// 首页长按确定可读完整字体授权；不占用首页底部空间。
// 对同一screen重复调用不重复创建；screen删除会清理主页时钟及附属阅读器。
// 须在持有 LVGL 锁的情况下调用(由组装层负责加锁)。
void ui_profile_create(void);
bool ui_profile_open_licenses(void);
bool ui_profile_is_active(void); // LVGL lock required

// 返回内置中文字体指针(void* 形式,实为 const lv_font_t*),供其它层叠加中文文字时复用,
// 避免调用方直接依赖 presentation 的字体符号 / lvgl 类型。
const void *ui_font_cn16(void);
const void *ui_font_cn24(void);

// 运行时更新各字段。注意:这些函数会操作 LVGL 对象,
// 若从 LVGL 任务之外调用,请用 platform_lvgl_lock()/unlock() 包起来。
// 昵称；当前字体缺失的汉字显示为 '*'，缺失的非汉字不显示；不修改持久化原值。
void ui_profile_set_name(const char *name);
// Safe from BLE host task; applied by the UI clock.
void ui_profile_set_ble_connected(bool connected);
// 电量:5 档电池图标 + 分段百分比;level 1..5,==1 红 / ==2 黄 / 其余白。
// percent<0 或 level<1 表示未知,显示灰色轮廓和 --%;有效 0% 保留 0 且显示红色。
void ui_profile_set_battery(int percent, int level);
// token 余额 + 上限:进度条 = token/token_max,文本 "token / token_max"。
void ui_profile_set_title(const char *title);
void ui_profile_set_intro(const char *intro);

// 在头像位显示图片(contain 缩放,常驻);dsc=NULL 则清空头像位(留空)。须在 LVGL 锁内调用。
// dsc 实为 const lv_image_dsc_t*,用 void* 避免此公共头依赖 lvgl。
void ui_profile_show_image(const void *dsc);

// dock 某项被"确定"时回调(item_id: 2游戏 3看图)
typedef void (*ui_action_cb_t)(int item_id, void *user);
void ui_profile_set_on_action(ui_action_cb_t cb, void *user);
