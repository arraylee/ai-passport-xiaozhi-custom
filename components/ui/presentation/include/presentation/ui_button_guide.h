// 开机/玩法扫码绑定页。所有函数须在持有 LVGL 锁时调用。
#pragma once
#include <stdbool.h>

// 显示指引页并记住当前页面。
void ui_button_guide_open(const char *sn, const char *key);
// Same QR payload and visuals, with a visible short-OK return to Plays.
void ui_button_guide_open_menu(const char *sn, const char *key);
// 绑定页包含密钥，截图策略在 LVGL 锁内查询，禁止远程截取。
bool ui_button_guide_active(void);

// 返回进入指引前的页面并销毁指引页。
void ui_button_guide_close(void);
