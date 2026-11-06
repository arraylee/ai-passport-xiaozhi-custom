#pragma once
enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_GPIO };
int esp_sleep_get_wakeup_cause(void);
