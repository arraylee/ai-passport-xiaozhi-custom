#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XZ_WIFI_PROFILE_MAX 5
#define XZ_WIFI_PROFILES_BLOB_SIZE 497

typedef struct {
    uint8_t ssid[32];
    uint8_t ssid_len;
    char password[64];
} xz_wifi_profile_t;

/* Item zero is the most recent successful connection. */
typedef struct {
    uint8_t count;
    xz_wifi_profile_t items[XZ_WIFI_PROFILE_MAX];
} xz_wifi_profiles_t;

/* Versioned, CRC32-protected encoding; independent of struct layout and endian. */
bool xz_wifi_profiles_decode(xz_wifi_profiles_t *out, const void *data, size_t size);
bool xz_wifi_profiles_encode(const xz_wifi_profiles_t *p, void *out, size_t capacity);

/* Call only after a successful connection. Invalid input leaves p unchanged.
 * SSID: 1..32 non-NUL bytes. Password: empty or 8..63 bytes plus a terminator.
 * A duplicate moves to the front with its new password; a sixth drops the oldest.
 */
bool xz_wifi_profiles_remember(xz_wifi_profiles_t *p, const uint8_t *ssid,
                               size_t len, const char *password);

/* RSSI INT8_MIN means unavailable. Prefer visible item zero, then strongest RSSI;
 * equal RSSI keeps recency order. Returns the number of indices written to out.
 * The unused portion of out is zeroed; invalid inputs return zero.
 */
size_t xz_wifi_profiles_rank(const xz_wifi_profiles_t *p,
                             const int8_t rssi[XZ_WIFI_PROFILE_MAX],
                             uint8_t out[XZ_WIFI_PROFILE_MAX]);

#ifdef __cplusplus
}
#endif
