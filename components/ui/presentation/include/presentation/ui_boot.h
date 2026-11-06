#pragma once
#include <stdbool.h>
#define UI_BOOT_DURATION_MS 3400u
// All calls run under the LVGL lock. Overlay stays visible until the actual
// XiaoZhi screen is ready, so the hidden legacy home never flashes at boot.
bool ui_boot_open(void);
bool ui_boot_drawn(void);
void ui_boot_close(void);
