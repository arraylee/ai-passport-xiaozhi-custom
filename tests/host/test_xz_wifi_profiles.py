"""Compile the portable saved-network policy and exercise untrusted NVS data."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
import zlib


ROOT = Path(__file__).resolve().parents[2]
CASES = r'''
#include "xz_wifi_profiles.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

enum { HEADER_SIZE = 8, RECORD_SIZE = 97, CRC_OFFSET = 493 };

static void remember(xz_wifi_profiles_t *p, const char *ssid, const char *password)
{
    assert(xz_wifi_profiles_remember(p, (const uint8_t *)ssid, strlen(ssid), password));
}

static void is_profile(const xz_wifi_profile_t *p, const char *ssid, const char *password)
{
    assert(p->ssid_len == strlen(ssid));
    assert(!memcmp(p->ssid, ssid, p->ssid_len));
    assert(!strcmp(p->password, password));
}

static void assert_zero(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < size; ++i) assert(bytes[i] == 0);
}

/* Independently reseal deliberately malformed records to reach format validation. */
static void reseal(uint8_t *blob)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < CRC_OFFSET; ++i) {
        crc ^= blob[i];
        for (unsigned b = 0; b < 8; ++b)
            crc = (crc & 1) ? (crc >> 1) ^ UINT32_C(0xedb88320) : crc >> 1;
    }
    crc ^= UINT32_MAX;
    for (unsigned i = 0; i < 4; ++i) blob[CRC_OFFSET + i] = (uint8_t)(crc >> (i * 8));
}

static void rejected_blob(const void *blob, size_t size)
{
    xz_wifi_profiles_t output;
    memset(&output, 0xa5, sizeof output);
    assert(!xz_wifi_profiles_decode(&output, blob, size));
    assert_zero(&output, sizeof output);
}

static void test_remember(void)
{
    xz_wifi_profiles_t p = {0};
    remember(&p, "A", "");
    remember(&p, "B", "password-B");
    remember(&p, "C", "password-C");
    remember(&p, "D", "password-D");
    remember(&p, "E", "password-E");
    assert(p.count == 5);
    is_profile(&p.items[0], "E", "password-E");
    is_profile(&p.items[4], "A", "");
    remember(&p, "B", "updated-B");
    assert(p.count == 5);
    is_profile(&p.items[0], "B", "updated-B");
    is_profile(&p.items[1], "E", "password-E");
    is_profile(&p.items[2], "D", "password-D");
    is_profile(&p.items[3], "C", "password-C");
    is_profile(&p.items[4], "A", "");
    remember(&p, "F", "password-F");
    assert(p.count == 5);
    is_profile(&p.items[0], "F", "password-F");
    is_profile(&p.items[4], "C", "password-C");
    remember(&p, "F", "");
    assert(p.count == 5);
    is_profile(&p.items[0], "F", "");

    /* Inputs may refer to the existing profile that is being moved. */
    assert(xz_wifi_profiles_remember(&p, p.items[3].ssid, p.items[3].ssid_len,
                                     p.items[3].password));
    is_profile(&p.items[0], "D", "password-D");
}

static void test_input_boundaries(void)
{
    xz_wifi_profiles_t p = {0}, before;
    uint8_t ssid[33];
    memset(ssid, 0xff, sizeof ssid);
    char password[65];
    memset(password, 'p', sizeof password);
    password[63] = 0;
    assert(xz_wifi_profiles_remember(&p, ssid, 32, password));
    assert(p.items[0].ssid_len == 32 && !memcmp(p.items[0].ssid, ssid, 32));
    assert(strlen(p.items[0].password) == 63);
    remember(&p, "\xe5\xae\xb6\xe9\x87\x8c", "12345678");
    assert(p.items[0].ssid_len == 6);
    assert(!memcmp(p.items[0].ssid, "\xe5\xae\xb6\xe9\x87\x8c", 6));
    before = p;
    assert(!xz_wifi_profiles_remember(NULL, ssid, 32, ""));
    assert(!xz_wifi_profiles_remember(&p, NULL, 1, ""));
    assert(!xz_wifi_profiles_remember(&p, ssid, 0, ""));
    assert(!xz_wifi_profiles_remember(&p, ssid, 33, ""));
    assert(!xz_wifi_profiles_remember(&p, ssid, 32, NULL));
    assert(!xz_wifi_profiles_remember(&p, ssid, 32, "1"));
    assert(!xz_wifi_profiles_remember(&p, ssid, 32, "1234567"));
    password[63] = 'p';
    password[64] = 0;
    assert(!xz_wifi_profiles_remember(&p, ssid, 32, password));
    char unterminated[64];
    memset(unterminated, 'p', sizeof unterminated);
    assert(!xz_wifi_profiles_remember(&p, ssid, 32, unterminated));
    ssid[15] = 0;
    assert(!xz_wifi_profiles_remember(&p, ssid, 32, ""));
    assert(!memcmp(&p, &before, sizeof p));
    p.count = 6;
    before = p;
    assert(!xz_wifi_profiles_remember(&p, (const uint8_t *)"Home", 4, ""));
    assert(!memcmp(&p, &before, sizeof p));
}

static void test_serialization(const char *fixture_path)
{
    xz_wifi_profiles_t p = {0}, decoded;
    uint8_t blob[XZ_WIFI_PROFILES_BLOB_SIZE + 2];
    memset(blob, 0xa5, sizeof blob);
    remember(&p, "Home", "test-password");
    remember(&p, "\xe5\xae\xb6\xe9\x87\x8c", "");
    assert(xz_wifi_profiles_encode(&p, blob + 1, XZ_WIFI_PROFILES_BLOB_SIZE));
    assert(blob[0] == 0xa5 && blob[sizeof blob - 1] == 0xa5);
    assert(xz_wifi_profiles_decode(&decoded, blob + 1, XZ_WIFI_PROFILES_BLOB_SIZE));
    assert(!memcmp(&p, &decoded, sizeof p));
    FILE *fixture = fopen(fixture_path, "wb");
    assert(fixture);
    assert(fwrite(blob + 1, 1, XZ_WIFI_PROFILES_BLOB_SIZE, fixture) == XZ_WIFI_PROFILES_BLOB_SIZE);
    assert(!fclose(fixture));

    /* Stale padding is never serialized; identical credentials have one encoding. */
    memset(p.items[0].ssid + 6, 0xd1, 26);
    memset(p.items[0].password + 1, 0xd2, 63);
    memset(p.items + p.count, 0xd3, sizeof p.items - p.count * sizeof p.items[0]);
    uint8_t canonical[XZ_WIFI_PROFILES_BLOB_SIZE];
    assert(xz_wifi_profiles_encode(&p, canonical, sizeof canonical));
    assert(!memcmp(canonical, blob + 1, sizeof canonical));

    /* Every byte, including the header, padding and checksum, is protected. */
    for (size_t i = 0; i < sizeof canonical; ++i) {
        canonical[i] ^= 1;
        rejected_blob(canonical, sizeof canonical);
        canonical[i] ^= 1;
    }
    for (size_t n = 0; n < sizeof canonical; ++n) rejected_blob(canonical, n);
    rejected_blob(blob, sizeof blob);
    rejected_blob(NULL, XZ_WIFI_PROFILES_BLOB_SIZE);
    assert(!xz_wifi_profiles_decode(NULL, canonical, sizeof canonical));

    memset(blob, 0xa5, sizeof blob);
    assert(!xz_wifi_profiles_encode(&p, blob + 1, XZ_WIFI_PROFILES_BLOB_SIZE - 1));
    assert(blob[1] == 0xa5);
    assert(!xz_wifi_profiles_encode(NULL, blob, sizeof blob));
    assert(!xz_wifi_profiles_encode(&p, NULL, sizeof blob));
    p.count = 6;
    assert(!xz_wifi_profiles_encode(&p, blob, sizeof blob));

    p = (xz_wifi_profiles_t){0};
    assert(xz_wifi_profiles_encode(&p, canonical, sizeof canonical));
    memset(&decoded, 0xa5, sizeof decoded);
    assert(xz_wifi_profiles_decode(&decoded, canonical, sizeof canonical));
    assert_zero(&decoded, sizeof decoded);
}

static void test_malformed_records(void)
{
    xz_wifi_profiles_t p = {0};
    uint8_t good[XZ_WIFI_PROFILES_BLOB_SIZE], bad[XZ_WIFI_PROFILES_BLOB_SIZE];
    remember(&p, "Home", "12345678");
    assert(xz_wifi_profiles_encode(&p, good, sizeof good));
    const size_t offsets[] = {0, 4, 6, 7, HEADER_SIZE, HEADER_SIZE + 2,
        HEADER_SIZE + 5, HEADER_SIZE + 33 + 1, HEADER_SIZE + 33 + 9,
        HEADER_SIZE + RECORD_SIZE};
    const uint8_t values[] = {'?', 2, 1, 1, 0, 0, 1, 0, 1, 1};
    for (size_t i = 0; i < sizeof offsets / sizeof offsets[0]; ++i) {
        memcpy(bad, good, sizeof bad);
        bad[offsets[i]] = values[i];
        reseal(bad);
        rejected_blob(bad, sizeof bad);
    }
    memcpy(bad, good, sizeof bad);
    bad[5] = 6;
    reseal(bad);
    rejected_blob(bad, sizeof bad);
    memcpy(bad, good, sizeof bad);
    bad[HEADER_SIZE] = 33;
    reseal(bad);
    rejected_blob(bad, sizeof bad);
    memcpy(bad, good, sizeof bad);
    memset(bad + HEADER_SIZE + 33, 'p', 64);
    reseal(bad);
    rejected_blob(bad, sizeof bad);
    memcpy(bad, good, sizeof bad);
    bad[5] = 2;
    memcpy(bad + HEADER_SIZE + RECORD_SIZE, bad + HEADER_SIZE, RECORD_SIZE);
    reseal(bad);
    rejected_blob(bad, sizeof bad);

    /* Invalid in-memory input is also rejected before it reaches NVS. */
    p.items[0].ssid_len = 0;
    assert(!xz_wifi_profiles_encode(&p, bad, sizeof bad));
    p.items[0].ssid_len = 4;
    memset(p.items[0].password, 'p', sizeof p.items[0].password);
    assert(!xz_wifi_profiles_encode(&p, bad, sizeof bad));
    strcpy(p.items[0].password, "1234567");
    assert(!xz_wifi_profiles_encode(&p, bad, sizeof bad));
    strcpy(p.items[0].password, "12345678");
    p.count = 2;
    p.items[1] = p.items[0];
    assert(!xz_wifi_profiles_encode(&p, bad, sizeof bad));
}

static void ranked(const xz_wifi_profiles_t *p, const int8_t *rssi,
                   const uint8_t *expected, size_t count)
{
    uint8_t result[7];
    memset(result, 0xa5, sizeof result);
    assert(xz_wifi_profiles_rank(p, rssi, result + 1) == count);
    assert(result[0] == 0xa5 && result[6] == 0xa5);
    assert(!memcmp(result + 1, expected, count));
    assert_zero(result + count + 1, 5 - count);
}

static void test_ranking(void)
{
    xz_wifi_profiles_t p = {0};
    remember(&p, "A", "");
    remember(&p, "B", "");
    remember(&p, "C", "");
    remember(&p, "D", "");
    remember(&p, "E", "");
    const int8_t preferred_weak[5] = {-90, -30, -60, -20, -128};
    const uint8_t preferred_order[] = {0, 3, 1, 2};
    ranked(&p, preferred_weak, preferred_order, 4);
    const int8_t preferred_missing[5] = {-128, -30, -60, -20, -128};
    const uint8_t fallback_order[] = {3, 1, 2};
    ranked(&p, preferred_missing, fallback_order, 3);
    const int8_t tied[5] = {-128, -40, -40, -40, -40};
    const uint8_t stable_order[] = {1, 2, 3, 4};
    ranked(&p, tied, stable_order, 4);
    const int8_t none[5] = {-128, -128, -128, -128, -128};
    ranked(&p, none, stable_order, 0);
    p.count = 2;
    const uint8_t bounded_order[] = {0, 1};
    ranked(&p, preferred_weak, bounded_order, 2);
    p.count = 0;
    ranked(&p, preferred_weak, bounded_order, 0);
    p.count = 6;
    ranked(&p, preferred_weak, bounded_order, 0);
    ranked(NULL, preferred_weak, bounded_order, 0);
    ranked(&p, NULL, bounded_order, 0);
    assert(!xz_wifi_profiles_rank(&p, preferred_weak, NULL));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    test_remember();
    test_input_boundaries();
    test_serialization(argv[1]);
    test_malformed_records();
    test_ranking();
    puts("saved Wi-Fi: capacity, recency, exact bytes, CRC, malformed input and fallback passed");
    return 0;
}
'''


class WifiProfilesTest(unittest.TestCase):
    def test_saved_network_policy_and_portable_format(self):
        component = ROOT / 'components/platform/xiaozhi'
        with tempfile.TemporaryDirectory() as folder:
            source = Path(folder) / 'profiles.c'
            executable = Path(folder) / 'profiles'
            fixture = Path(folder) / 'profiles.bin'
            source.write_text(CASES)
            compiled = subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                 '-fsanitize=undefined', '-fno-sanitize-recover=all',
                 '-I', str(component), str(source),
                 str(component / 'xz_wifi_profiles.c'), '-o', str(executable)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(executable), str(fixture)], check=True, timeout=10,
                           env={**os.environ, 'UBSAN_OPTIONS': 'halt_on_error=1'})

            # Python's CRC implementation independently checks the complete wire format.
            expected = bytearray(497)
            expected[:8] = b'XZWP\x01\x02\x00\x00'
            expected[8] = 6
            expected[9:15] = '家里'.encode()
            expected[105] = 4
            expected[106:110] = b'Home'
            expected[138:151] = b'test-password'
            expected[-4:] = zlib.crc32(expected[:-4]).to_bytes(4, 'little')
            self.assertEqual(fixture.read_bytes(), expected)


if __name__ == '__main__':
    unittest.main()
