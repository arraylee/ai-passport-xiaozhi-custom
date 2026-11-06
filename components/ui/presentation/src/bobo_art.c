/* Generated native descriptors; PTP1 bytes stay in mapped Flash. */
#include "presentation/bobo_art.h"
#include "presentation/pet_art_pack.h"
#if LV_BIN_DECODER_RAM_LOAD
#error "Native sprites require row decoding"
#endif
const char bobo_art_pack_fingerprint[] = "PETART-SHA256:831ea05cbde18614a6f748fd2f4926fcda772b3a3af0559b48b3ffb0c56a7d36";
static bool ready;
lv_image_dsc_t dodge_art_books = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 48, .h = 64 } };
lv_image_dsc_t bobo_art_walk_left = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 40, .h = 40 } };
lv_image_dsc_t bobo_art_wave = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 80, .h = 80 } };
lv_image_dsc_t dodge_art_luggage = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 48, .h = 64 } };
lv_image_dsc_t dodge_art_robot = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 48, .h = 64 } };
lv_image_dsc_t bobo_art_walk_right = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 40, .h = 40 } };
lv_image_dsc_t dodge_art_plant = { .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, .w = 48, .h = 64 } };
static const pet_art_asset_t catalog[] = {
    { 0x15853983u, 48, 64, 2255u, 0xbbcb757fu, &dodge_art_books },
    { 0x416f5b4fu, 40, 40, 1347u, 0xf9eb394du, &bobo_art_walk_left },
    { 0x6fc0e5e9u, 80, 80, 3778u, 0x45b46876u, &bobo_art_wave },
    { 0x7870361du, 48, 64, 2531u, 0xdb97259au, &dodge_art_luggage },
    { 0x7fda8f9du, 48, 64, 2130u, 0x08ac8a01u, &dodge_art_robot },
    { 0x94aa27c0u, 40, 40, 1342u, 0x65d046bau, &bobo_art_walk_right },
    { 0xdfbadb4cu, 48, 64, 2448u, 0xf14aae09u, &dodge_art_plant },
};
#define ASSET_COUNT (sizeof(catalog) / sizeof(catalog[0]))
bool bobo_art_bind(const void *data, size_t size) {
    ready = pet_art_pack_bind(data, size, catalog, ASSET_COUNT, bobo_art_pack_fingerprint);
    return ready;
}
void bobo_art_unbind(void) { pet_art_pack_unbind(catalog, ASSET_COUNT); ready = false; }
bool bobo_art_ready(void) { return ready; }
size_t bobo_art_asset_count(void) { return ASSET_COUNT; }
const pet_art_asset_t *bobo_art_asset(size_t index) {
    return index < ASSET_COUNT ? &catalog[index] : NULL;
}
