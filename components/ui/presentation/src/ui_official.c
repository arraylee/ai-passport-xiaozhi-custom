#include "presentation/ui_official.h"
#include "presentation/companion_resources.h"
#include "presentation/ui_theme.h"
#include "presentation/ui_xiaozhi_theme.h"
#include "src/draw/lv_image_decoder_private.h"
#include "xz_theme.h"
#include "xz_theme_font.h"
#include <string.h>

static xz_theme_font_t packed_font;
static lv_font_t text_font;
static const uint8_t *font_source;
static lv_image_dsc_t face_image, background;
static unsigned face_edge = XZ_ICON_EDGE;
static int face_top;
static bool face_rows;
// External packs do not align their RGB565 data. Copy the visible part of one
// row to an aligned drawing buffer before the C3's renderer reads 16-bit pixels.
static lv_result_t wallpaper_info(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *d,
                                  lv_image_header_t *h) {
  (void)decoder;
  if (d->src_type != LV_IMAGE_SRC_VARIABLE ||
      (d->src != &background && d->src != &face_image))
    return LV_RESULT_INVALID;
  const lv_image_dsc_t *src = d->src;
  if (!src->data || (src == &background && src->data_size != 240*320*2))
    return LV_RESULT_INVALID;
  *h = src->header;
  h->cf = src == &background ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB565A8;
  return LV_RESULT_OK;
}
static lv_result_t wallpaper_open(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *d) {
  (void)decoder;
  if (d->src != &background && d->src != &face_image)
    return LV_RESULT_INVALID;
  d->user_data = lv_draw_buf_create(d->header.w, 1, d->header.cf, LV_STRIDE_AUTO);
  if (!d->user_data)
    return LV_RESULT_INVALID;
  d->args.no_cache = true;
  d->decoded = NULL;
  return LV_RESULT_OK;
}
static lv_result_t wallpaper_row(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *d,
                                 const lv_area_t *area, lv_area_t *result) {
  (void)decoder;
  if (!area || !result || area->x1 < 0 || area->x2 >= d->header.w || area->x2 < area->x1 || area->y1 < 0 ||
      area->y2 >= d->header.h || area->y2 < area->y1)
    return LV_RESULT_INVALID;
  int y = result->y1 == LV_COORD_MIN ? area->y1 : result->y2 + 1;
  if (y < area->y1 || y > area->y2)
    return LV_RESULT_INVALID;
  unsigned width = area->x2 - area->x1 + 1;
  lv_draw_buf_t *row =
      lv_draw_buf_reshape(d->user_data, d->header.cf, width, 1, LV_STRIDE_AUTO);
  if (!row)
    return LV_RESULT_INVALID;
  if (d->src == &background)
    memcpy(row->data, background.data + 480 * y + 2 * area->x1, 2 * width);
  else {
    // Scale from mapped Flash one visible row at a time. LVGL must not allocate
    // an entire transformed 128x128 image while voice/network buffers are live.
    unsigned sy = y * face_edge / d->header.h;
    for (unsigned x = 0; x < width; x++) {
      unsigned sx = (area->x1 + x) * face_edge / d->header.w, i = sy * face_edge + sx;
      memcpy(row->data + 2*x, face_image.data + 2*i, 2);
      row->data[row->header.stride + x] = face_image.data[2*face_edge*face_edge + i];
    }
  }
  if (d->args.flush_cache)
    lv_draw_buf_flush_cache(row, NULL);
  d->decoded = row;
  *result = (lv_area_t){area->x1, y, area->x2, y};
  return LV_RESULT_OK;
}
static void wallpaper_close(lv_image_decoder_t *decoder, lv_image_decoder_dsc_t *d) {
  (void)decoder;
  lv_draw_buf_destroy(d->user_data);
  d->user_data = NULL;
  d->decoded = NULL;
}
static bool wallpaper_decoder(void) {
  for (lv_image_decoder_t *it = lv_image_decoder_get_next(NULL); it;
       it = lv_image_decoder_get_next(it))
    if (it->info_cb == wallpaper_info)
      return true;
  lv_image_decoder_t *d = lv_image_decoder_create();
  if (!d)
    return false;
  lv_image_decoder_set_info_cb(d, wallpaper_info);
  lv_image_decoder_set_open_cb(d, wallpaper_open);
  lv_image_decoder_set_get_area_cb(d, wallpaper_row);
  lv_image_decoder_set_close_cb(d, wallpaper_close);
  return true;
}
static bool glyph(const lv_font_t *font, lv_font_glyph_dsc_t *out, uint32_t cp, uint32_t next) {
  (void)font;
  (void)next;
  xz_theme_glyph_t g;
  if (!xz_theme_glyph(&packed_font, cp, &g))
    return false;
  out->gid.index = cp;
  out->adv_w = g.advance;
  out->box_w = g.width;
  out->box_h = g.height;
  out->ofs_x = g.x;
  out->ofs_y = g.y;
  out->is_placeholder = false;
  out->format = LV_FONT_GLYPH_FORMAT_A8;
  return true;
}
static const void *pixels(lv_font_glyph_dsc_t *d, lv_draw_buf_t *b) {
  xz_theme_glyph_t g;
  if (!b || !b->data || d->req_raw_bitmap || b->header.cf != LV_COLOR_FORMAT_A8 ||
      !xz_theme_glyph(&packed_font, d->gid.index, &g) || b->header.w < g.width ||
      b->header.h < g.height || !b->header.stride || b->data_size / b->header.stride < g.height ||
      !xz_theme_pixels(&packed_font, &g, b->data, b->header.stride))
    return NULL;
  return b;
}
const lv_font_t *ui_official_font(void) {
  const xz_theme_t *t = xz_theme_installed();
  if (!t || !t->font.data)
    return &ui_font_body;
  if (t->font.data != font_source) {
    if (!xz_theme_font_open(t->font, &packed_font))
      return &ui_font_body;
    text_font = (lv_font_t){.get_glyph_dsc = glyph,
                            .get_glyph_bitmap = pixels,
                            .line_height = packed_font.height,
                            .base_line = packed_font.baseline,
                            .fallback = &ui_font_body};
    font_source = t->font.data;
  }
  return &text_font;
}
static const xz_skin_t *skin(void) {
  const xz_theme_t *t = xz_theme_installed();
  return t ? &t->skin[xz_theme_dark()] : NULL;
}
uint32_t ui_official_ink(void) {
  const xz_skin_t *s = skin();
  return s ? s->ink : (xz_theme_dark() ? XZ_UI_INK : 0x24363b);
}
uint32_t ui_official_background(void) {
  const xz_skin_t *s = skin();
  return s ? s->background : (xz_theme_dark() ? XZ_UI_PANEL : 0xecf5f1);
}
lv_obj_t *ui_official_stage(lv_obj_t *root, int y) {
  // Only an explicit wallpaper needs a viewport. Transparent expressions are
  // direct children of the screen, including when a pack supplies only colors.
  const xz_skin_t *s = skin();
  face_rows = wallpaper_decoder();
  face_top = y;
  if (s && s->image.data && face_rows) {
    lv_obj_t *stage = lv_obj_create(root);
    lv_obj_remove_style_all(stage);
    lv_obj_set_pos(stage, 12, y);
    lv_obj_set_size(stage, 216, 144);
    lv_obj_remove_flag(stage, LV_OBJ_FLAG_SCROLLABLE);
    background = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                                             .cf = LV_COLOR_FORMAT_RAW,
                                             .w = 240,
                                             .h = 320,
                                             .stride = 480},
                                  .data = s->image.data,
                                  .data_size = s->image.size};
    lv_obj_t *wallpaper = lv_image_create(stage);
    lv_image_set_src(wallpaper, &background);
    lv_obj_set_pos(wallpaper, -12, -y);
  }
  lv_obj_t *image = lv_image_create(root);
  lv_obj_align(image, LV_ALIGN_TOP_MID, 0, y + (144 - XZ_ICON_EDGE) / 2);
  face_image = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                                           .cf = face_rows ? LV_COLOR_FORMAT_RAW : LV_COLOR_FORMAT_RGB565A8,
                                           .w = XZ_ICON_EDGE,
                                           .h = XZ_ICON_EDGE,
                                           .stride = 2 * XZ_ICON_EDGE},
                                .data_size = XZ_ICON_BYTES};
  return image;
}
unsigned ui_official_expression(lv_obj_t *image, const xz_view_t *v, uint32_t elapsed) {
  unsigned e = v->emotion < XZ_EMOTIONS ? v->emotion : 0;
  if (v->phase == XZ_THINK)
    e = 11;
  else if (v->phase == XZ_STANDBY)
    e = 18;
  else if (v->phase == XZ_ERROR)
    e = 4;
  else if (v->phase != XZ_SPEAK)
    e = (v->phase == XZ_READY || v->phase == XZ_FAREWELL) ? 1 : 0;
  unsigned delay = 0;
  const uint8_t *p = xz_theme_icon(e, elapsed, &delay);
  unsigned display_edge = XZ_ICON_EDGE;
  unsigned edge = p ? xz_theme_icon_edge(e, &display_edge) : XZ_ICON_EDGE;
  if (!p) {
    unsigned idx = e == 11                        ? 2
                   : e == 18                      ? 3
                   : (e == 4 || e == 5 || e == 6) ? 4
                   : (e == 9 || e == 10)          ? 5
                   : e                            ? 1
                                                  : 0;
    p = companion_official_icon(idx);
  }
  if (!face_rows)
    display_edge = edge;
  if (p && (p != face_image.data || display_edge != face_image.header.w)) {
    face_edge = edge;
    face_image.header.w = face_image.header.h = display_edge;
    face_image.header.stride = 2 * display_edge;
    face_image.data_size = 3 * edge * edge;
    face_image.data = p;
    lv_image_set_src(image, &face_image);
    lv_obj_align(image, LV_ALIGN_TOP_MID, 0, face_top + (144 - display_edge) / 2);
    lv_obj_invalidate(image);
  }
  return delay;
}
