#pragma once
#include <stdbool.h>
bool platform_lvgl_lock(int timeout_ms);
void platform_lvgl_unlock(void);

bool platform_ble_voice_pause(void);
bool platform_ble_voice_resume(void);
