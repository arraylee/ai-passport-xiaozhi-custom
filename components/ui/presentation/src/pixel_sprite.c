#include "presentation/pixel_sprite.h"
#include "src/draw/lv_image_decoder_private.h"
#include <string.h>

typedef struct {
    const uint8_t *palette, *offsets, *stream;
    uint16_t width, height, stream_size;
    uint8_t colors;
} sprite_data_t;
typedef struct {
    lv_draw_buf_t row;
    sprite_data_t sprite;
    uint8_t pixels[];
} sprite_session_t;

static uint16_t read16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static bool layout(const lv_image_dsc_t *image, sprite_data_t *sprite) {
    if (!image || image->header.magic != LV_IMAGE_HEADER_MAGIC ||
        image->header.cf != LV_COLOR_FORMAT_RAW_ALPHA || image->header.flags != PIXEL_SPRITE_FLAG ||
        image->header.stride || image->header.reserved_2 ||
        !image->header.w || image->header.w > PIXEL_SPRITE_MAX_WIDTH ||
        !image->header.h || image->header.h > PIXEL_SPRITE_MAX_HEIGHT ||
        !image->data || image->data_size < 8) return false;
    const uint8_t *data = image->data;
    if (memcmp(data, "PSR1", 4) || !data[4] || data[4] > 64 || data[5] || data[6] || data[7]) return false;
    uint32_t prefix = 8u + data[4] * 4u + ((uint32_t)image->header.h + 1u) * 2u;
    if (prefix >= image->data_size || image->data_size - prefix > UINT16_MAX) return false;
    *sprite = (sprite_data_t){
        .palette = data + 8, .offsets = data + 8 + data[4] * 4u, .stream = data + prefix,
        .width = image->header.w, .height = image->header.h,
        .stream_size = (uint16_t)(image->data_size - prefix), .colors = data[4]
    };
    if (read16(sprite->offsets) || read16(sprite->offsets + sprite->height * 2u) != sprite->stream_size) return false;
    for (unsigned y = 0; y < sprite->height; ++y) {
        unsigned begin = read16(sprite->offsets + y * 2u), end = read16(sprite->offsets + (y + 1u) * 2u);
        if (begin >= end || end > sprite->stream_size) return false;
    }
    return true;
}

/* A null output validates a row without allocating. The same bounded parser
 * handles rendering, including corrupt pixels outside the requested crop. */
static bool read_row(const sprite_data_t *sprite, unsigned y, unsigned left, unsigned right,
                     lv_color32_t *output, bool premultiply) {
    if (y >= sprite->height || left > right || right >= sprite->width) return false;
    unsigned begin = read16(sprite->offsets + y * 2u), end = read16(sprite->offsets + (y + 1u) * 2u);
    if (begin >= end || end > sprite->stream_size) return false;
    unsigned position = begin, x = 0;
    while (position < end) {
        unsigned tag = sprite->stream[position++], count = (tag & 127u) + 1u;
        unsigned bytes = tag & 128u ? 1u : count;
        if (count > sprite->width - x || bytes > end - position) return false;
        for (unsigned i = 0; i < count; ++i) {
            unsigned index = sprite->stream[position + (tag & 128u ? 0u : i)];
            if (index >= sprite->colors) return false;
            if (output && x + i >= left && x + i <= right) {
                lv_color32_t color;
                memcpy(&color, sprite->palette + index * 4u, sizeof(color));
                if (premultiply) lv_color_premultiply(&color);
                output[x + i - left] = color;
            }
        }
        position += bytes; x += count;
    }
    return x == sprite->width;
}
bool pixel_sprite_validate(const lv_image_dsc_t *image) {
    sprite_data_t sprite;
    if (!layout(image, &sprite)) return false;
    for (unsigned y = 0; y < sprite.height; ++y)
        if (!read_row(&sprite, y, 0, sprite.width - 1u, NULL, false)) return false;
    return true;
}

static lv_result_t sprite_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                               lv_image_header_t *header) {
    (void)decoder;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE || !dsc->src) return LV_RESULT_INVALID;
    const lv_image_dsc_t *image = dsc->src;
    /* Claim malformed PSR1 assets too: returning INVALID here would let the
     * generic BIN decoder treat compressed bytes as a directly usable image. */
    bool ours = (image->header.flags & PIXEL_SPRITE_FLAG) ||
        (image->header.cf == LV_COLOR_FORMAT_RAW_ALPHA && image->data && image->data_size >= 4 &&
         !memcmp(image->data, "PSR1", 4));
    if (!ours) return LV_RESULT_INVALID;
    *header = (lv_image_header_t){.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888,
        .w = image->header.w && image->header.w <= PIXEL_SPRITE_MAX_WIDTH ? image->header.w : 1,
        .h = image->header.h && image->header.h <= PIXEL_SPRITE_MAX_HEIGHT ? image->header.h : 1};
    header->stride = header->w * 4u;
    return LV_RESULT_OK;
}
static lv_result_t sprite_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    const lv_image_dsc_t *image = dsc->src;
    sprite_data_t sprite;
    if (!layout(image, &sprite)) return LV_RESULT_INVALID;
    for (unsigned y = 0; y < sprite.height; ++y)
        if (!read_row(&sprite, y, 0, sprite.width - 1u, NULL, false)) return LV_RESULT_INVALID;
    uint32_t bytes = lv_draw_buf_width_to_stride(sprite.width, LV_COLOR_FORMAT_ARGB8888) + LV_DRAW_BUF_ALIGN;
    sprite_session_t *session = lv_malloc(sizeof(*session) + bytes);
    if (!session) return LV_RESULT_INVALID;
    session->sprite = sprite;
    if (lv_draw_buf_init(&session->row, sprite.width, 1, LV_COLOR_FORMAT_ARGB8888,
                         LV_STRIDE_AUTO, session->pixels, bytes) != LV_RESULT_OK) {
        lv_free(session); return LV_RESULT_INVALID;
    }
    dsc->user_data = session;
    dsc->decoded = NULL; /* Force LVGL's incremental get_area drawing path. */
    dsc->args.no_cache = true;
    return LV_RESULT_OK;
}
static lv_result_t sprite_area(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc,
                               const lv_area_t *full, lv_area_t *decoded) {
    (void)decoder;
    sprite_session_t *session = dsc->user_data;
    if (!session || !full || !decoded || full->x1 < 0 || full->y1 < 0 ||
        full->x2 < full->x1 || full->y2 < full->y1 || full->x2 >= session->sprite.width ||
        full->y2 >= session->sprite.height) return LV_RESULT_INVALID;
    int32_t y = full->y1;
    if (decoded->y1 != LV_COORD_MIN) {
        if (decoded->x1 != full->x1 || decoded->x2 != full->x2 || decoded->y1 != decoded->y2 ||
            decoded->y1 < full->y1 || decoded->y1 >= full->y2) return LV_RESULT_INVALID;
        y = decoded->y1 + 1;
    }
    lv_draw_buf_t *row = lv_draw_buf_reshape(&session->row, LV_COLOR_FORMAT_ARGB8888,
                                             (uint32_t)(full->x2 - full->x1 + 1), 1, LV_STRIDE_AUTO);
    if (!row || !read_row(&session->sprite, (unsigned)y, (unsigned)full->x1, (unsigned)full->x2,
                          (lv_color32_t *)row->data, dsc->args.premultiply)) return LV_RESULT_INVALID;
    row->header.flags = LV_IMAGE_FLAGS_MODIFIABLE | (dsc->args.premultiply ? LV_IMAGE_FLAGS_PREMULTIPLIED : 0);
    if (dsc->args.flush_cache) lv_draw_buf_flush_cache(row, NULL);
    *decoded = (lv_area_t){.x1 = full->x1, .y1 = y, .x2 = full->x2, .y2 = y};
    dsc->decoded = row;
    return LV_RESULT_OK;
}
static void sprite_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *dsc) {
    (void)decoder;
    lv_free(dsc->user_data); dsc->user_data = NULL; dsc->decoded = NULL;
}
lv_result_t pixel_sprite_init(void) {
    for (lv_image_decoder_t *decoder = lv_image_decoder_get_next(NULL); decoder;
         decoder = lv_image_decoder_get_next(decoder))
        if (decoder->info_cb == sprite_info) return LV_RESULT_OK;
    lv_image_decoder_t *decoder = lv_image_decoder_create();
    if (!decoder) return LV_RESULT_INVALID;
    decoder->name = "PSR1";
    lv_image_decoder_set_info_cb(decoder, sprite_info);
    lv_image_decoder_set_open_cb(decoder, sprite_open);
    lv_image_decoder_set_get_area_cb(decoder, sprite_area);
    lv_image_decoder_set_close_cb(decoder, sprite_close);
    return LV_RESULT_OK;
}
