#pragma once
static inline void fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
