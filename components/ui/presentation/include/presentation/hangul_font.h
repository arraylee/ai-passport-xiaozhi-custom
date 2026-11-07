#pragma once
#include "lvgl.h"
LV_FONT_DECLARE(passport_ko_16);
void passport_languages_bind(const uint8_t *data, size_t size);
LV_FONT_DECLARE(passport_ko_20);
void passport_hangul_bind(const uint8_t *body, size_t body_size, const uint8_t *title,
                          size_t title_size);
