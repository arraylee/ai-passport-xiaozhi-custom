#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* PTP1 v1 uses a 64-byte little-endian header and 24-byte index entries.
 * See tools/pet_art_pack.py for the canonical disk layout. CRC-32 detects
 * accidental damage; it does not authenticate a pack from an untrusted party.
 * Binding allocates no heap and makes two passes, exposing data only after
 * every index, catalog value, payload CRC, and PSR1 row has been checked. */
#define PET_ART_PACK_CAPACITY 0x100000u
#define PET_ART_PACK_MAX_COUNT 1024u
typedef struct {
    uint32_t id;
    uint16_t width, height;
    uint32_t size, crc32;
    lv_image_dsc_t *image;
} pet_art_asset_t;

/* Source memory must remain readable/immutable until all image users stop.
 * Call before UI creation, never during an active draw/decoder session.
 * Failure clears every supplied descriptor, including a previous valid bind.
 * The compiled catalog's fingerprint is retained in the app for release-time
 * SHA-256 pairing. Runtime integrity uses exact catalog metadata and CRCs. */
bool pet_art_pack_bind(const void *data, size_t size, const pet_art_asset_t *catalog,
                       size_t count, const char *fingerprint);
void pet_art_pack_unbind(const pet_art_asset_t *catalog, size_t count);
