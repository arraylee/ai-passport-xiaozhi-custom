#pragma once
#include "lvgl.h"
#include "xiaozhi.h"
const lv_font_t *ui_official_font(void);
uint32_t ui_official_ink(void);
uint32_t ui_official_background(void);
lv_obj_t *ui_official_stage(lv_obj_t *root, int y);
unsigned ui_official_expression(lv_obj_t *image, const xz_view_t *view, uint32_t elapsed);
