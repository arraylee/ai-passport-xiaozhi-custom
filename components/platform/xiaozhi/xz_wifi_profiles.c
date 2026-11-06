#include "xz_wifi_profiles.h"

#include <string.h>

enum {
    BLOB_HEADER_SIZE = 8,
    BLOB_RECORD_SIZE = 97,
    BLOB_CRC_OFFSET = BLOB_HEADER_SIZE + XZ_WIFI_PROFILE_MAX * BLOB_RECORD_SIZE,
    BLOB_VERSION = 1
};

_Static_assert(BLOB_CRC_OFFSET + 4 == XZ_WIFI_PROFILES_BLOB_SIZE, "Wi-Fi blob size");

static const uint8_t blob_magic[4] = {'X', 'Z', 'W', 'P'};

static bool password_length(const char *password, size_t *length)
{
    if (!password) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (password[i] == '\0') {
            if (i != 0 && i < 8) return false;
            *length = i;
            return true;
        }
    }
    return false;
}

static bool valid_ssid(const uint8_t *ssid, size_t length)
{
    return ssid && length > 0 && length <= 32 && !memchr(ssid, 0, length);
}

static bool same_ssid(const xz_wifi_profile_t *a, const xz_wifi_profile_t *b)
{
    return a->ssid_len == b->ssid_len && !memcmp(a->ssid, b->ssid, a->ssid_len);
}

static bool valid_profiles(const xz_wifi_profiles_t *p)
{
    if (!p || p->count > XZ_WIFI_PROFILE_MAX) return false;
    for (size_t i = 0; i < p->count; ++i) {
        const xz_wifi_profile_t *profile = &p->items[i];
        size_t length;
        if (!valid_ssid(profile->ssid, profile->ssid_len) ||
            !password_length(profile->password, &length)) return false;
        for (size_t j = 0; j < i; ++j)
            if (same_ssid(profile, &p->items[j])) return false;
    }
    return true;
}

static bool all_zero(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i)
        if (data[i] != 0) return false;
    return true;
}

static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return crc ^ UINT32_MAX;
}

bool xz_wifi_profiles_encode(const xz_wifi_profiles_t *p, void *out, size_t capacity)
{
    if (!out || capacity < XZ_WIFI_PROFILES_BLOB_SIZE || !valid_profiles(p)) return false;
    uint8_t blob[XZ_WIFI_PROFILES_BLOB_SIZE] = {0};
    memcpy(blob, blob_magic, sizeof blob_magic);
    blob[4] = BLOB_VERSION;
    blob[5] = p->count;
    for (size_t i = 0; i < p->count; ++i) {
        const xz_wifi_profile_t *profile = &p->items[i];
        uint8_t *record = blob + BLOB_HEADER_SIZE + i * BLOB_RECORD_SIZE;
        size_t length = 0;
        (void)password_length(profile->password, &length);
        record[0] = profile->ssid_len;
        memcpy(record + 1, profile->ssid, profile->ssid_len);
        memcpy(record + 33, profile->password, length);
    }
    const uint32_t crc = crc32(blob, BLOB_CRC_OFFSET);
    for (unsigned i = 0; i < 4; ++i) blob[BLOB_CRC_OFFSET + i] = (uint8_t)(crc >> (8 * i));
    memcpy(out, blob, sizeof blob);
    return true;
}

bool xz_wifi_profiles_decode(xz_wifi_profiles_t *out, const void *data, size_t size)
{
    if (!out) return false;
    xz_wifi_profiles_t decoded = {0};
    const uint8_t *blob = data;
    if (!blob || size != XZ_WIFI_PROFILES_BLOB_SIZE ||
        memcmp(blob, blob_magic, sizeof blob_magic) || blob[4] != BLOB_VERSION ||
        blob[5] > XZ_WIFI_PROFILE_MAX || blob[6] || blob[7]) goto invalid;
    uint32_t stored_crc = 0;
    for (unsigned i = 0; i < 4; ++i)
        stored_crc |= (uint32_t)blob[BLOB_CRC_OFFSET + i] << (8 * i);
    if (stored_crc != crc32(blob, BLOB_CRC_OFFSET)) goto invalid;
    decoded.count = blob[5];
    for (size_t i = 0; i < XZ_WIFI_PROFILE_MAX; ++i) {
        const uint8_t *record = blob + BLOB_HEADER_SIZE + i * BLOB_RECORD_SIZE;
        if (i >= decoded.count) {
            if (!all_zero(record, BLOB_RECORD_SIZE)) goto invalid;
            continue;
        }
        xz_wifi_profile_t *profile = &decoded.items[i];
        profile->ssid_len = record[0];
        if (!valid_ssid(record + 1, profile->ssid_len) ||
            !all_zero(record + 1 + profile->ssid_len, 32 - profile->ssid_len)) goto invalid;
        memcpy(profile->ssid, record + 1, profile->ssid_len);
        memcpy(profile->password, record + 33, sizeof profile->password);
        size_t password_size;
        if (!password_length(profile->password, &password_size) ||
            !all_zero(record + 33 + password_size, 64 - password_size)) goto invalid;
    }
    if (!valid_profiles(&decoded)) goto invalid;
    *out = decoded;
    return true;

invalid:
    memset(out, 0, sizeof *out);
    return false;
}

bool xz_wifi_profiles_remember(xz_wifi_profiles_t *p, const uint8_t *ssid,
                               size_t len, const char *password)
{
    size_t password_size;
    if (!valid_profiles(p) || !valid_ssid(ssid, len) ||
        !password_length(password, &password_size)) return false;
    xz_wifi_profile_t profile = {0};
    profile.ssid_len = (uint8_t)len;
    memcpy(profile.ssid, ssid, len);
    memcpy(profile.password, password, password_size);

    size_t position = 0;
    while (position < p->count && !same_ssid(&p->items[position], &profile)) ++position;
    if (position == p->count) {
        if (p->count < XZ_WIFI_PROFILE_MAX) ++p->count;
        else position = XZ_WIFI_PROFILE_MAX - 1;
    }
    memmove(p->items + 1, p->items, position * sizeof p->items[0]);
    p->items[0] = profile;
    return true;
}

size_t xz_wifi_profiles_rank(const xz_wifi_profiles_t *p,
                             const int8_t rssi[XZ_WIFI_PROFILE_MAX],
                             uint8_t out[XZ_WIFI_PROFILE_MAX])
{
    if (!out) return 0;
    memset(out, 0, XZ_WIFI_PROFILE_MAX);
    if (!rssi || !valid_profiles(p)) return 0;
    size_t count = 0;
    if (p->count > 0 && rssi[0] != INT8_MIN) out[count++] = 0;
    const size_t preferred = count;
    for (size_t i = 1; i < p->count; ++i) {
        if (rssi[i] == INT8_MIN) continue;
        size_t position = count;
        while (position > preferred && rssi[i] > rssi[out[position - 1]]) {
            out[position] = out[position - 1];
            --position;
        }
        out[position] = (uint8_t)i;
        ++count;
    }
    return count;
}
