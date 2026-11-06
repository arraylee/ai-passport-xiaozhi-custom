// Real LVGL objects + production lifecycle functions. The decode target also
// compiles the complete production JPEG path; only codec/Flash/heap boundaries
// are controlled, so its checks cover real allocation and cleanup decisions.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lvgl.h"
#include "platform/platform_factory.h"
#include "jpeg_store.h"
#include "avatar_store.h"

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(guard) ((void)(guard))
#define portEXIT_CRITICAL(guard) ((void)(guard))
static void fake_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
#define ESP_LOGI(...) fake_log(__VA_ARGS__)
#define ESP_LOGW(...) fake_log(__VA_ARGS__)
#ifdef JPEG_VIEW_REAL_DECODE
#include "esp_heap_caps.h"
#include "../../../components/platform/platform_esp32/src/jpeg_view.c"
#else
static int decode_to_flash(int *width, int *height);
// Generated directly from jpeg_view.c on every source change, including real
// globals, build_dsc, show_image, teardown, tick and public request/callback APIs.
#include "jpeg_view_production.inc"
#endif

static lv_obj_t *home, *avatar;
static bool can_show = true;
static unsigned decode_calls, mmap_calls, unmap_calls;
static bool native_available;
static uint8_t native_pixels[96*156*4];
static unsigned displayed, deleted, live_photos;
static unsigned result_calls, result_ok, result_fail, avatar_restores, avatar_clears;
static bool fail_decode, fail_map, mapped, exit_during_decode;
static uint8_t pixels[240 * 320 * 4];
#ifdef JPEG_VIEW_REAL_DECODE
static unsigned stripe_allocs, stripe_frees, stripe_allocation_failures;
static unsigned decoder_closes, frame_begins, frame_writes, png_calls;
static void *stripe;
static size_t stripe_bytes;
static bool fail_alloc, fail_begin, fail_write, fail_process;
static bool decoder_open;
static int codec_outlen = 7680, codec_step;
static unsigned source_maps, source_unmaps;
static const uint8_t jpeg_bytes[] = {0xff, 0xd8, 0xff, 0xda, 0, 2, 0xff, 0xd9};
static const uint8_t png_bytes[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
#endif

static void check(bool condition, const char *failure) {
    if (!condition) { fprintf(stderr, "FAIL: %s\n", failure); exit(1); }
}
#ifndef JPEG_VIEW_REAL_DECODE
static int decode_to_flash(int *width, int *height) {
    decode_calls++;
    if (exit_during_decode && s_req_mode == JPEG_VIEW_FULLSCREEN) jpeg_view_exit();
    // A replacement reuses imgframe, so the previous mapping may already be
    // invalid by the time this operation reports an error.
    mapped = false;
    if (fail_decode) return -1;
    *width = s_req_mode == JPEG_VIEW_AVATAR ? 96 : 240;
    *height = s_req_mode == JPEG_VIEW_AVATAR ? 156 : 320;
    s_frame_cf = s_req_mode == JPEG_VIEW_AVATAR ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_RGB565;
    return 0;
}
#else
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps) {
    check(stripe == NULL, "decode retained an earlier JPEG stripe allocation");
    check(s_decoding && decoder_open, "JPEG stripe allocated outside its decode operation");
    check(alignment == 16 && caps == MALLOC_CAP_8BIT,
          "JPEG stripe allocation lost its C3 byte-addressable alignment contract");
    check(size > 0 && size <= 7680, "JPEG allocation exceeds the validated stripe bound");
    stripe_allocs++;
    if (fail_alloc) { stripe_allocation_failures++; return NULL; }
    void *pointer = NULL;
    check(posix_memalign(&pointer, alignment, size) == 0, "host stripe allocation failed");
    stripe = pointer; stripe_bytes = size;
    return pointer;
}
void heap_caps_free(void *pointer) {
    if (!pointer) return;
    check(pointer == stripe, "JPEG cleanup freed an unowned stripe");
    check(!decoder_open, "JPEG cleanup released its stripe before closing the decoder");
    free(pointer); stripe = NULL; stripe_bytes = 0; stripe_frees++;
}
int jpeg_store_mmap(jpeg_slot_t slot, const uint8_t **pointer, int *length) {
    (void)slot; source_maps++; *pointer = jpeg_bytes; *length = sizeof jpeg_bytes; return 0;
}
void jpeg_store_unmap(void) {
    check(!decoder_open && !stripe, "JPEG source unmapped before decode resources were released");
    source_unmaps++;
}
int avatar_store_get(const char *name, const uint8_t **pointer, int *length) {
    (void)name; *pointer = png_bytes; *length = sizeof png_bytes; return 0;
}
int png_decode_to_frame(const uint8_t *pointer, int length, int *width, int *height) {
    check(pointer == png_bytes && length == sizeof png_bytes, "PNG decoder input changed");
    png_calls++; decode_calls++; *width = 96; *height = 156;
    mapped = false; return fail_decode ? -1 : 0;
}
jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config, jpeg_dec_handle_t *decoder) {
    check(config->block_enable && config->output_type == JPEG_PIXEL_FORMAT_RGB565_LE,
          "JPEG production path changed its block/pixel format");
    decode_calls++; codec_step = 0;
    check(!decoder_open, "JPEG decoder leaked across operations"); decoder_open = true;
    if (exit_during_decode && s_req_mode == JPEG_VIEW_FULLSCREEN) jpeg_view_exit();
    *decoder = &codec_step; return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t decoder, jpeg_dec_io_t *io,
                                  jpeg_dec_header_info_t *info) {
    check(decoder == &codec_step && io->inbuf == jpeg_bytes, "JPEG codec input changed");
    info->width = s_req_mode == JPEG_VIEW_AVATAR ? 96 : 240;
    info->height = s_req_mode == JPEG_VIEW_AVATAR ? 160 : 320;
    return fail_decode ? JPEG_ERR_BAD_DATA : JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t decoder, int *length) {
    (void)decoder; *length = codec_outlen; return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_get_process_count(jpeg_dec_handle_t decoder, int *count) {
    (void)decoder; *count = s_req_mode == JPEG_VIEW_AVATAR ? 10 : 20; return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t decoder, jpeg_dec_io_t *io) {
    (void)decoder;
    int length = (s_req_mode == JPEG_VIEW_AVATAR ? 96 : 240) * 16 * 2;
    check(((uintptr_t)io->outbuf & 15) == 0, "JPEG codec received an unaligned output buffer");
    if (stripe) check(io->outbuf == stripe && (size_t)length <= stripe_bytes,
                      "JPEG codec did not use its live allocated output buffer");
    if (fail_process) { io->out_size = 0; return JPEG_ERR_BAD_DATA; }
    memset(io->outbuf, ++codec_step, length); io->out_size = length; return JPEG_ERR_OK;
}
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t decoder) {
    check(decoder == &codec_step, "wrong JPEG decoder closed");
    check(decoder_open, "JPEG decoder closed twice");
    decoder_open = false; decoder_closes++; return JPEG_ERR_OK;
}
int jpeg_frame_begin(uint32_t bytes) {
    check(stripe && decoder_open, "JPEG frame erased before its output stripe was available");
    check(bytes <= sizeof pixels, "JPEG frame exceeds backing Flash");
    frame_begins++; mapped = false;
    return fail_begin ? -1 : 0;
}
int jpeg_frame_write(uint32_t offset, const uint8_t *pointer, int length) {
    check(pointer == stripe && decoder_open && (size_t)length <= stripe_bytes,
          "JPEG frame write outlived or exceeded its decode stripe");
    check(offset + length <= sizeof pixels, "JPEG write exceeds frame storage");
    frame_writes++;
    if (fail_write) return -1;
    memcpy(pixels + offset, pointer, length); return 0;
}
#endif
int jpeg_frame_mmap(const uint8_t **pointer, uint32_t bytes) {
    mmap_calls++;
    check(bytes <= sizeof pixels, "frame mapping exceeds test backing storage");
    if (fail_map) return -1;
    mapped = true; *pointer = pixels; return 0;
}
void jpeg_frame_unmap(void) {
    // These are actual LVGL objects: a hidden old screen is unsafe too, since
    // returning to it would draw from the already invalidated shared mapping.
    check(live_photos == 0, "Flash unmapped while an old photo screen still exists");
    check(avatar == NULL, "Flash unmapped before the home avatar released its image");
    mapped = false; unmap_calls++;
}
bool avatar_store_has(const char *name) { return strcmp(name, "default") == 0; }
int avatar_store_get_native(const char *name,const uint8_t **pixels,size_t *size) {
    (void)name;*pixels=NULL;*size=0;
    if(!native_available)return 1;
    *pixels=native_pixels;*size=sizeof native_pixels;return 0;
}
bool jpeg_store_has_valid(jpeg_slot_t slot) { return slot == JPEG_SLOT_FULL; }
static bool allowed(void *user) { assert(user == &can_show); return can_show; }
static void result(int ok, void *user) {
    (void)user; result_calls++;
    if (ok) result_ok++; else result_fail++;
}
static void photo_deleted(lv_event_t *event) {
    (void)event; check(live_photos > 0, "duplicate photo deletion");
    live_photos--; deleted++;
}
static void photo_shown(void *screen, void *user) {
    (void)user; lv_obj_t *photo = screen;
    check(lv_obj_is_valid(photo) && lv_obj_get_parent(photo) == NULL,
          "screen callback received an invalid screen");
    check(lv_obj_get_child_count(photo) == 1, "photo screen is missing its image");
    lv_obj_t *image = lv_obj_get_child(photo, 0);
    check(lv_image_get_src(image) == &s_dsc && mapped, "photo does not refer to the decoded frame");
    lv_obj_add_event_cb(photo, photo_deleted, LV_EVENT_DELETE, NULL);
    displayed++; live_photos++;
}
static void set_avatar(const void *descriptor, void *user) {
    (void)user;
    if (avatar) { lv_obj_delete(avatar); avatar = NULL; }
    if (descriptor) {
        check(mapped || ((const lv_image_dsc_t *)descriptor)->data==native_pixels,
              "avatar restored from an unmapped frame");
        avatar = lv_image_create(home); lv_image_set_src(avatar, descriptor); avatar_restores++;
    } else avatar_clears++;
}
static void setup(void) {
    lv_init();
    lv_display_t *display = lv_display_create(240, 320); assert(display);
    home = lv_screen_active(); assert(home);
    jpeg_view_set_can_show_cb(allowed, &can_show);
    jpeg_view_set_screen_cb(photo_shown, NULL);
    jpeg_view_set_avatar_cb(set_avatar, NULL);
    jpeg_view_set_result_cb(result, NULL);
    jpeg_view_request_avatar("default"); tick(NULL);
    check(avatar && mapped && avatar_restores == 1, "initial home avatar did not load");
    check(result_calls == 0 && s_prev == NULL, "initial avatar changed fullscreen ownership or reported a transfer");
}
static lv_obj_t *open_photo(void) {
    jpeg_view_request(); tick(NULL);
    check(s_active && s_full_pending == 0 && lv_screen_active() == s_scr, "photo did not become the active screen");
    check(s_prev == home && s_scr != home, "fullscreen return target is not the original home");
    return s_scr;
}
static uint16_t draw_buffer[240 * 20];
static unsigned home_flushes;
static void return_flush(lv_display_t *display, const lv_area_t *area, uint8_t *data) {
    (void)area; (void)data;
    if (lv_screen_active() == home) {
        check(avatar && (mapped || s_dsc.data==native_pixels) && lv_image_get_src(avatar) == &s_dsc &&
              s_dsc.header.w == 96 && s_dsc.header.h == 156,
              "first home refresh exposed the placeholder before restoring the saved avatar");
        home_flushes++;
    }
    lv_display_flush_ready(display);
}
static void seamless_return(void) {
    lv_display_t *display = lv_display_get_default();
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof draw_buffer,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, return_flush);
    open_photo(); lv_refr_now(NULL);
    jpeg_view_exit(); tick(NULL);
    lv_refr_now(NULL);
    check(home_flushes && !s_pending && !s_active && avatar_restores == 2,
          "photo return did not finish with the restored home avatar");
    check(result_calls == 1 && result_ok == 1,
          "internal return restore sent a phantom image-transfer result");
}
static void native_avatar_return(void) {
    native_available=true;
    unsigned decoded=decode_calls, maps=mmap_calls;
    jpeg_view_request_avatar("default");tick(NULL);
    check(s_dsc.data==native_pixels && s_dsc.header.cf==LV_COLOR_FORMAT_ARGB8888,
          "native avatar did not use immutable transparent frame");
    check(decode_calls==decoded && mmap_calls==maps,
          "native avatar unnecessarily allocated/decompressed a PNG frame");
    lv_display_t *display=lv_display_get_default();
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,draw_buffer,NULL,sizeof draw_buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,return_flush);
    for(unsigned i=0;i<50;i++) {
        decoded=decode_calls;maps=mmap_calls;
#ifdef JPEG_VIEW_REAL_DECODE
        unsigned allocations = stripe_allocs, frees = stripe_frees;
#endif
        open_photo();lv_refr_now(NULL);jpeg_view_exit();tick(NULL);lv_refr_now(NULL);
        check(s_dsc.data==native_pixels && !s_pending && !s_active && !mapped,
              "photo return did not restore native avatar in the same frame");
        check(decode_calls==decoded+1 && mmap_calls==maps+1,
              "return performed an extra avatar decode/map");
#ifdef JPEG_VIEW_REAL_DECODE
        check(stripe_allocs == allocations + 1 && stripe_frees == frees + 1 && !stripe,
              "photo/native-avatar cycle leaked a stripe or allocated on native restore");
#endif
    }
}
static void deferred(void) {
    // Model each blocking owner independently: Plays, binding QR, and an
    // authorization screen. A blocked tick cannot capture any of them as prev.
    can_show = false;
    jpeg_view_request();
    unsigned decodes = decode_calls, maps = mmap_calls;
    for (unsigned owner = 0; owner < 3; owner++) {
        lv_obj_t *occupied = lv_obj_create(NULL); lv_screen_load(occupied);
        tick(NULL); tick(NULL);
        check(decode_calls == decodes && mmap_calls == maps && displayed == 0,
              "blocked fullscreen request decoded or displayed behind an owning screen");
        check(s_full_pending == 1 && s_prev == NULL && s_scr == NULL,
              "blocked tick consumed the pending request or captured the temporary screen");
        check(result_calls == 0 && lv_screen_active() == occupied && avatar,
              "blocked request changed the current UI or reported success");
        lv_screen_load(home); lv_obj_delete(occupied);
    }
    can_show = true; tick(NULL);
    check(decode_calls == decodes + 1 && displayed == 1 && s_prev == home,
          "unblocked request did not display once with a stable home return target");
    lv_obj_t *photo = s_scr;
    tick(NULL); tick(NULL);
    check(s_full_pending == 0 && decode_calls == decodes + 1 && displayed == 1 && result_calls == 1,
          "unblocked fullscreen request was decoded or reported more than once");
    check(lv_screen_active() == photo && lv_obj_is_valid(photo), "shown photo did not remain active");
}
static void replacement(void) {
    lv_obj_t *first = open_photo();
    lv_obj_t *second = open_photo();
    check(second != first && lv_obj_is_valid(second) && !lv_obj_is_valid(first),
          "replacement did not delete the actual old LVGL photo screen");
    check(s_prev == home && lv_obj_is_valid(home) && lv_screen_active() == second,
          "replacement retained an old photo as its return destination");
    check(displayed == 2 && deleted == 1 && live_photos == 1 && result_ok == 2,
          "replacement leaked a photo screen or emitted the wrong display result");
    jpeg_view_exit(); tick(NULL);
    check(lv_screen_active() == home && !lv_obj_is_valid(second) && live_photos == 0,
          "leaving a replaced photo did not return safely to home");
    check(!s_pending && avatar && unmap_calls == 1,
          "normal exit did not restore the avatar in the same LVGL transaction");
    tick(NULL);
    check(avatar && avatar_restores == 2 && result_calls == 2,
          "internal avatar restore emitted an extra image transfer result");
}
static void failed_replacement(bool mapping_failure) {
    lv_obj_t *first = open_photo();
    fail_decode = !mapping_failure; fail_map = mapping_failure;
    jpeg_view_request(); tick(NULL);
    check(!lv_obj_is_valid(first) && live_photos == 0 && s_scr == NULL,
          "failed replacement left the old LVGL photo referencing invalid frame memory");
    check(lv_screen_active() == home && lv_obj_is_valid(home) && !s_active,
          "failed replacement did not return to the original home screen");
    check(avatar == NULL && avatar_clears == 1 && unmap_calls == 1 && !mapped,
          "failed replacement did not release old photo and avatar references before unmapping");
    check(s_pending == 1 && s_restore_only == 1,
          "failed replacement lost the queued internal avatar recovery");
    check(result_calls == 2 && result_ok == 1 && result_fail == 1,
          "failed replacement did not report exactly one failure for the requested photo");
    fail_decode = fail_map = false; tick(NULL);
    check(avatar && mapped && avatar_restores == 2 && lv_screen_active() == home && !s_pending,
          "next tick did not restore the home avatar after the failed photo");
    check(result_calls == 2, "internal recovery reported an unsolicited image transfer result");
}
static void blocked_full_with_avatar(void) {
    lv_obj_t *occupied = lv_obj_create(NULL); lv_screen_load(occupied);
    can_show = false; jpeg_view_request(); tick(NULL);
    unsigned decodes = decode_calls;
    jpeg_view_request_avatar("default"); tick(NULL);
    check(decode_calls == decodes + 1 && avatar_restores == 2 && s_req_mode == JPEG_VIEW_AVATAR,
          "avatar did not decode while the full photo waited for a stable return screen");
    check(s_full_pending == 1 && s_pending == 0 && displayed == 0 && s_prev == NULL,
          "an intervening avatar update consumed the blocked full-photo request");
    check(lv_screen_active() == occupied && result_calls == 0,
          "blocked-photo avatar update changed the owning screen or sent a photo result");
    lv_screen_load(home); lv_obj_delete(occupied); can_show = true; tick(NULL); tick(NULL);
    check(s_full_pending == 0 && s_active && displayed == 1 && s_prev == home,
          "full photo was lost after an avatar update during its wait");
    check(decode_calls == decodes + 2 && result_calls == 1 && result_ok == 1,
          "unblocked full photo did not decode and report exactly once after avatar update");
}
static void active_avatar(bool uploaded) {
    lv_obj_t *photo = open_photo();
    unsigned decodes = decode_calls;
    if (uploaded) jpeg_view_request_mode(JPEG_VIEW_AVATAR);
    else jpeg_view_request_avatar("default");
    tick(NULL); tick(NULL);
    check(decode_calls == decodes && s_pending == 1 && s_active && mapped,
          "avatar update overwrote the shared frame while a photo was visible");
    check(lv_screen_active() == photo && lv_obj_is_valid(photo) && s_req_mode == JPEG_VIEW_FULLSCREEN,
          "queued avatar changed the visible photo or current decode mode");
    jpeg_view_exit(); tick(NULL);
    check(!lv_obj_is_valid(photo) && lv_screen_active() == home && !s_pending && avatar,
          "exiting the photo did not apply the queued avatar before returning home");
    tick(NULL);
    check(avatar && avatar_restores == 2 && decode_calls == decodes + 1 && !s_pending,
          "deferred avatar did not decode once after photo exit");
    check(result_calls == (uploaded ? 2u : 1u) && result_fail == 0,
          "explicit uploaded avatar lost its result or named avatar sent an unsolicited result");
}
static void first_failure(bool mapping_failure) {
    fail_decode = !mapping_failure; fail_map = mapping_failure;
    jpeg_view_request(); tick(NULL);
    check(lv_screen_active() == home && s_scr == NULL && !s_active && displayed == 0,
          "failed first photo left fullscreen ownership active");
    check(avatar == NULL && avatar_clears == 1 && unmap_calls == 1 && !mapped,
          "failed first photo left the existing home avatar on invalid frame memory");
    check(s_pending == 1 && s_restore_only == 1 && s_full_pending == 0,
          "failed first photo did not queue internal home-avatar recovery");
    check(result_calls == 1 && result_fail == 1 && result_ok == 0,
          "failed first photo did not report exactly one requested failure");
    fail_decode = fail_map = false; tick(NULL);
    check(avatar && mapped && avatar_restores == 2 && !s_pending && result_calls == 1,
          "first-photo failure recovery did not restore avatar silently");
}
static void exit_race(int mode) {
    if (mode != 2) open_photo();
    unsigned photos = displayed;
    jpeg_view_request();
    if (mode == 1) exit_during_decode = true;
    else jpeg_view_exit();
    tick(NULL);
    check(lv_screen_active() == home && !s_active && !s_pending && !s_full_pending,
          "exit lost to queued or decoding photo");
    check(avatar && mapped, "exit exposed a missing home avatar for a frame");
    check(displayed == photos + 1, "replacement decode result was lost during exit");
    tick(NULL);
    check(lv_screen_active() == home && !s_exit_req, "exit left stale ownership");
    exit_during_decode = false;
    open_photo();
    check(s_active, "fresh explicit request after exit was lost");
}
#ifdef JPEG_VIEW_REAL_DECODE
static void stripe_success(void) {
    open_photo();
    check(stripe_allocs == 1 && stripe_frees == 1 && !stripe,
          "successful JPEG display retained its decode stripe");
    check(decoder_closes == 1 && source_maps == 1 && source_unmaps == 1,
          "successful JPEG display retained its decoder or source mapping");
    check(frame_begins == 1 && frame_writes == 20 && s_dsc.data_size == 153600,
          "JPEG frame dimensions or strip count changed");
    for (unsigned offset = 0; offset < 153600; offset++)
        check(pixels[offset] == offset / 7680 + 1, "JPEG RGB565 strip contents or write order changed");
}
static void stripe_failure(const char *scenario) {
    bool allocation_failure = !strcmp(scenario, "stripe_alloc_failure");
    bool bad_outlen = !strcmp(scenario, "stripe_zero_outlen") ||
                      !strcmp(scenario, "stripe_oversize_outlen");
    fail_alloc = allocation_failure;
    fail_begin = !strcmp(scenario, "stripe_begin_failure");
    fail_write = !strcmp(scenario, "stripe_write_failure");
    fail_process = !strcmp(scenario, "stripe_process_failure");
    fail_decode = !strcmp(scenario, "stripe_header_failure");
    if (!strcmp(scenario, "stripe_zero_outlen")) codec_outlen = 0;
    if (!strcmp(scenario, "stripe_oversize_outlen")) codec_outlen = 7681;
    jpeg_view_request(); tick(NULL);
    check(result_fail == 1 && result_ok == 0 && !s_active && s_scr == NULL,
          "JPEG decode failure was displayed or reported as successful");
    check(decoder_closes == 1 && source_maps == 1 && source_unmaps == 1 && !s_decoding,
          "JPEG failure retained a decoder, source mapping or busy state");
    check(!stripe && stripe_frees == (allocation_failure || bad_outlen || fail_decode ? 0u : 1u),
          "JPEG failure retained or double-freed its decode stripe");
    check(stripe_allocs == (bad_outlen || fail_decode ? 0u : 1u),
          "JPEG allocated before validating its header and stripe size");
    if (allocation_failure || bad_outlen || fail_decode)
        check(frame_begins == 0 && frame_writes == 0,
              "JPEG decode erased/wrote frame Flash before it had a usable stripe");
    if (fail_begin || fail_process)
        check(frame_writes == 0, "JPEG wrote a frame after begin/process failure");
    check(avatar == NULL && !mapped && s_pending && s_restore_only && live_photos == 0,
          "JPEG allocation/decode failure skipped display cleanup and silent avatar recovery");
    fail_alloc = fail_begin = fail_write = fail_process = fail_decode = false;
    codec_outlen = 7680;
    tick(NULL);
    check(avatar && mapped && !s_pending && result_calls == 1,
          "JPEG decode failure did not recover its home avatar silently");
    unsigned allocations = stripe_allocs, frees = stripe_frees;
    open_photo();
    check(stripe_allocs == allocations + 1 && stripe_frees == frees + 1 && !stripe,
          "JPEG decode could not allocate and release a fresh stripe after failure");
}
static void stripe_png_unchanged(void) {
    unsigned decodes = decode_calls;
    jpeg_view_request_avatar("default"); tick(NULL);
    check(png_calls == 2 && decode_calls == decodes + 1 && avatar && mapped,
          "legacy PNG avatar path changed");
    check(stripe_allocs == 0 && stripe_frees == 0 && decoder_closes == 0,
          "PNG avatar unexpectedly allocated a JPEG stripe or opened a JPEG decoder");
    native_available = true;
    jpeg_view_request_avatar("default"); tick(NULL);
    check(s_dsc.data == native_pixels && stripe_allocs == 0 && stripe_frees == 0,
          "native avatar unexpectedly allocated a JPEG stripe");
}
#endif
int main(int argc, char **argv) {
    assert(argc == 2); setup();
#ifdef JPEG_VIEW_REAL_DECODE
    if (!strcmp(argv[1], "stripe_success")) stripe_success();
    else if (!strcmp(argv[1], "stripe_png_unchanged")) stripe_png_unchanged();
    else if (!strncmp(argv[1], "stripe_", 7)) stripe_failure(argv[1]);
    else
#endif
    if (!strcmp(argv[1], "native_avatar_return")) native_avatar_return();
    else if (!strcmp(argv[1], "exit_queued")) exit_race(0);
    else if (!strcmp(argv[1], "exit_decoding")) exit_race(1);
    else if (!strcmp(argv[1], "exit_before_show")) exit_race(2);
    else if (!strcmp(argv[1], "seamless_return")) seamless_return();
    else if (!strcmp(argv[1], "deferred")) deferred();
    else if (!strcmp(argv[1], "replace")) replacement();
    else if (!strcmp(argv[1], "decode_failure")) failed_replacement(false);
    else if (!strcmp(argv[1], "mmap_failure")) failed_replacement(true);
    else if (!strcmp(argv[1], "blocked_full_with_avatar")) blocked_full_with_avatar();
    else if (!strcmp(argv[1], "active_named_avatar")) active_avatar(false);
    else if (!strcmp(argv[1], "active_uploaded_avatar")) active_avatar(true);
    else if (!strcmp(argv[1], "first_decode_failure")) first_failure(false);
    else if (!strcmp(argv[1], "first_mmap_failure")) first_failure(true);
    else { fprintf(stderr, "Unknown scenario: %s\n", argv[1]); return 2; }
#ifdef JPEG_VIEW_REAL_DECODE
    check(!stripe && stripe_allocs == stripe_frees + stripe_allocation_failures,
          "scenario ended with an unreleased JPEG stripe");
#endif
    check(lv_mem_test() == LV_RESULT_OK, "LVGL heap integrity failed");
    printf("jpeg_view lifecycle: %s passed with real LVGL screen objects\n", argv[1]);
    return 0;
}
