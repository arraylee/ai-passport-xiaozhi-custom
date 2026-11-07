#pragma once
#include "lvgl.h"
enum { COMPANION_COUNT = 2, COMPANION_FACE_WIDTH = 144, COMPANION_FACE_HEIGHT = 144 };
typedef enum {
    COMPANION_IDLE, COMPANION_BLINK, COMPANION_LISTEN, COMPANION_THINK,
    COMPANION_TALK, COMPANION_HAPPY, COMPANION_FACE_COUNT
} companion_face_t;
/* Six consecutive native frames, mapped from read-only Flash. */
typedef struct { const lv_image_dsc_t *frames; } companion_portrait_t;
bool companion_resources_init(void);
const uint8_t *companion_official_icon(unsigned index);
const uint8_t *companion_home_avatar(void);
const companion_portrait_t *companion_avatar(unsigned index);
