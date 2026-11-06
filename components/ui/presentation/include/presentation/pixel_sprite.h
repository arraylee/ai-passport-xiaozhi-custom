#pragma once
#include "lvgl.h"

/* PSR1: immutable, Flash-resident indexed sprites decoded one row at a time.
 * lv_image_dsc_t: magic=LV_IMAGE_HEADER_MAGIC, cf=LV_COLOR_FORMAT_RAW_ALPHA,
 * flags=PIXEL_SPRITE_FLAG, native w/h, stride=0, data_size=exact payload bytes.
 *
 * Payload (all multibyte integers little endian):
 *   0..3  "PSR1"
 *   4     palette count, 1..64 (32 or 64 recommended for native pet artwork)
 *   5..7  zero, reserved
 *   8..   palette_count entries of B,G,R,A (straight alpha / LE 0xAARRGGBB)
 *         (height+1) u16 row offsets relative to the following RLE stream
 *         row-local RLE stream, at most 65535 bytes
 *
 * tag 0..127: (tag+1) literal palette-index bytes follow.
 * tag 128..255: ((tag&127)+1) copies of the one following palette index.
 * Row offsets must strictly increase from zero to the exact stream length.
 * Each row must consume its entire byte range and expand to exactly w pixels.
 * Every index, including pixels outside a requested crop, is validated.
 * Dimensions are bounded to the 240x320 device; no full-image cache is used.
 * Source descriptors and payloads must remain valid and immutable while used.
 */
#define PIXEL_SPRITE_FLAG LV_IMAGE_FLAGS_USER1
#define PIXEL_SPRITE_MAX_WIDTH 240u
#define PIXEL_SPRITE_MAX_HEIGHT 320u

/* Call after lv_init(), under the LVGL lock. Idempotent, including after a
 * fresh lv_deinit()/lv_init() cycle. Returns INVALID if registration fails. */
lv_result_t pixel_sprite_init(void);

/* Full allocation-free format validation, useful for generated asset checks. */
bool pixel_sprite_validate(const lv_image_dsc_t *image);
