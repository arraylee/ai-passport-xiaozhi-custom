#include "presentation/compact_face.h"
#include "lvgl.h"
#include "src/draw/lv_image_decoder_private.h"
#include <stdlib.h>
#include <string.h>

#define STRIPS (XZ_FACE_HEIGHT / XZ_FACE_ROWS)
#define HEADER_BYTES (4u + 2u * (STRIPS + 1u))

static unsigned read16(const uint8_t *p) {
    return p[0] | ((unsigned)p[1] << 8);
}

static bool plane(const uint8_t **p, const uint8_t *end, uint8_t *out, unsigned unit) {
    unsigned remaining = XZ_FACE_WIDTH * XZ_FACE_ROWS;
    while (remaining) {
        if (*p >= end) return false;
        unsigned control = *(*p)++;
        unsigned count = (control & 127u) + 1;
        unsigned input = (control & 128u) ? unit : count * unit;
        if (count > remaining || (size_t)(end - *p) < input) return false;
        if (!(control & 128u)) {
            memcpy(out, *p, input);
        } else if (unit == 1) {
            memset(out, **p, count);
        } else {
            for (unsigned i = 0; i < count; ++i) {
                out[2*i] = (*p)[0];
                out[2*i+1] = (*p)[1];
            }
        }
        *p += input;
        out += count * unit;
        remaining -= count;
    }
    return true;
}

bool compact_face_decode(const uint8_t *data, size_t bytes, unsigned strip,
                         uint8_t out[XZ_FACE_STRIP_BYTES]) {
    if (!data || !out || bytes < HEADER_BYTES || strip >= STRIPS || memcmp(data, "XZF1", 4))
        return false;
    unsigned lo = read16(data + 4 + strip * 2);
    unsigned hi = read16(data + 6 + strip * 2);
    if (lo < HEADER_BYTES || hi <= lo || hi > bytes) return false;
    const uint8_t *p = data + lo, *end = data + hi;
    return plane(&p, end, out, 2) &&
           plane(&p, end, out + 2 * XZ_FACE_WIDTH * XZ_FACE_ROWS, 1) && p == end;
}

typedef struct {
    lv_draw_buf_t buffer;
    uint8_t pixels[XZ_FACE_STRIP_BYTES] __attribute__((aligned(LV_DRAW_BUF_ALIGN)));
} strip_buffer_t;

static lv_result_t face_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                             lv_image_header_t *header) {
    (void)decoder;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;
    const lv_image_dsc_t *img = dsc->src;
    if (img->header.cf != LV_COLOR_FORMAT_RAW_ALPHA || !(img->header.flags & LV_IMAGE_FLAGS_USER1) ||
        img->header.w != XZ_FACE_WIDTH || img->header.h != XZ_FACE_HEIGHT ||
        !img->data || img->data_size < HEADER_BYTES || memcmp(img->data,"XZF1",4))
        return LV_RESULT_INVALID;
    *header = img->header;
    header->cf = LV_COLOR_FORMAT_RGB565A8;
    header->stride = XZ_FACE_WIDTH * 2;
    return LV_RESULT_OK;
}

static lv_result_t face_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    strip_buffer_t *work = malloc(sizeof(*work));
    if (!work) return LV_RESULT_INVALID;
    if (lv_draw_buf_init(&work->buffer, XZ_FACE_WIDTH, XZ_FACE_ROWS, LV_COLOR_FORMAT_RGB565A8,
                         2 * XZ_FACE_WIDTH, work->pixels, sizeof(work->pixels)) != LV_RESULT_OK) {
        free(work);
        return LV_RESULT_INVALID;
    }
    dsc->user_data = work;
    dsc->decoded = NULL;
    return LV_RESULT_OK;
}

static lv_result_t face_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                             const lv_area_t *window, lv_area_t *area) {
    (void)decoder;
    int y = area->y1 == LV_COORD_MIN ? window->y1 : area->y2 + 1;
    if (y < 0 || y > window->y2 || y >= XZ_FACE_HEIGHT) return LV_RESULT_INVALID;
    unsigned strip = (unsigned)y / XZ_FACE_ROWS;
    const lv_image_dsc_t *img = dsc->src;
    strip_buffer_t *work = dsc->user_data;
    if (!compact_face_decode(img->data, img->data_size, strip, work->pixels)) return LV_RESULT_INVALID;
    *area = (lv_area_t){0, (int)(strip * XZ_FACE_ROWS), XZ_FACE_WIDTH - 1,
                        (int)((strip + 1) * XZ_FACE_ROWS) - 1};
    dsc->decoded = &work->buffer;
    return LV_RESULT_OK;
}

static void face_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    free(dsc->user_data);
    dsc->decoded = NULL;
    dsc->user_data = NULL;
}

bool compact_face_register(void) {
    static lv_image_decoder_t *decoder;
    if (decoder) return true;
    decoder = lv_image_decoder_create();
    if (!decoder) return false;
    lv_image_decoder_set_info_cb(decoder, face_info);
    lv_image_decoder_set_open_cb(decoder, face_open);
    lv_image_decoder_set_get_area_cb(decoder, face_area);
    lv_image_decoder_set_close_cb(decoder, face_close);
    return true;
}
