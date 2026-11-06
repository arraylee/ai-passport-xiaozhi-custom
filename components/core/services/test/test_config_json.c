#include "unity.h"
#include "services/config_json.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

TEST_CASE("json partial update changes only present keys", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *j = "{\"name\":\"NEO\",\"level\":7,\"token\":1580}";
    cfg_changed_t c;
    int n = config_json_apply(&d, j, (int)strlen(j), &c);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("NEO", d.name);
    TEST_ASSERT_EQUAL_INT(7, d.level);
    TEST_ASSERT_EQUAL_INT(1580, d.token);
    TEST_ASSERT_TRUE(c.name && c.level && c.token);
    TEST_ASSERT_FALSE(c.role);
    TEST_ASSERT_EQUAL_STRING("NETRUNNER", d.role);
}

TEST_CASE("json parses token_max and clamps to >=1", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    cfg_changed_t c;
    const char *j = "{\"token\":100,\"token_max\":8888}";
    int n = config_json_apply(&d, j, (int)strlen(j), &c);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_TRUE(c.token && c.token_max);
    TEST_ASSERT_EQUAL_INT(100, d.token);
    TEST_ASSERT_EQUAL_INT(8888, d.token_max);
    const char *j2 = "{\"token_max\":0}";
    config_json_apply(&d, j2, (int)strlen(j2), NULL);
    TEST_ASSERT_EQUAL_INT(1, d.token_max);
}

TEST_CASE("json clamps out-of-range numbers", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *j = "{\"battery\":250,\"level\":-5,\"xp_max\":0,\"token\":-9}";
    int n = config_json_apply(&d, j, (int)strlen(j), NULL);
    TEST_ASSERT_TRUE(n >= 3);
    TEST_ASSERT_EQUAL_INT(100, d.battery);
    TEST_ASSERT_EQUAL_INT(0, d.level);
    TEST_ASSERT_EQUAL_INT(1, d.xp_max);
    TEST_ASSERT_EQUAL_INT(0, d.token);
}

TEST_CASE("json wrong-type key is skipped", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *j = "{\"battery\":\"high\",\"online\":true}";
    cfg_changed_t c;
    int n = config_json_apply(&d, j, (int)strlen(j), &c);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_FALSE(c.battery);
    TEST_ASSERT_TRUE(c.online);
    TEST_ASSERT_EQUAL_INT(82, d.battery);
}

TEST_CASE("json no known key returns 0", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *j = "{\"unknown\":1}";
    TEST_ASSERT_EQUAL_INT(0, config_json_apply(&d, j, (int)strlen(j), NULL));
}

TEST_CASE("json bad or non-object returns -1", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, "not json", 8, NULL));
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, "[1,2]", 5, NULL));
}

TEST_CASE("json nickname overrides legacy name and is UTF8 safe", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *j = "{\"name\":\"legacy\",\"nickname\":\"昵称\"}";
    cfg_changed_t c;
    TEST_ASSERT_EQUAL_INT(1, config_json_apply(&d, j, (int)strlen(j), &c));
    TEST_ASSERT_EQUAL_STRING("昵称", d.name);
    TEST_ASSERT_TRUE(c.name);
}

TEST_CASE("json accepts bounded avatar names and skips invalid ones", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d); cfg_changed_t c;
    const char *good = "{\"avatar_name\":\"violet_owl\"}";
    TEST_ASSERT_EQUAL_INT(1, config_json_apply(&d, good, (int)strlen(good), &c));
    TEST_ASSERT_TRUE(c.avatar_name);
    TEST_ASSERT_EQUAL_STRING("violet_owl", d.avatar_name);
    const char *bad = "{\"avatar_name\":\"Bad Avatar Name\"}";
    TEST_ASSERT_EQUAL_INT(0, config_json_apply(&d, bad, (int)strlen(bad), &c));
    TEST_ASSERT_FALSE(c.avatar_name);
    TEST_ASSERT_EQUAL_STRING("violet_owl", d.avatar_name);
}

TEST_CASE("json title and intro use canonical keys and intro caps at 28 codepoints", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d); cfg_changed_t c;
    const char *j = "{\"title\":\"工程师\",\"intro\":\"一二三四五六七八九十一二三四五六七八九十一二三四五六七八九十\"}";
    TEST_ASSERT_EQUAL_INT(2, config_json_apply(&d, j, (int)strlen(j), &c));
    TEST_ASSERT_TRUE(c.role && c.subtitle);
    TEST_ASSERT_EQUAL_STRING("工程师", d.role);
    TEST_ASSERT_EQUAL_UINT32(84, strlen(d.subtitle));
}

TEST_CASE("json rejects malformed UTF8 atomically in every string field", "[cfgjson]") {
    const char *keys[] = {"nickname", "name", "title", "role", "intro", "subtitle", "avatar_name", "img_mode"};
    const char *invalid[] = {"\xf0", "\xf0\x90", "\xf0\x90\x80", "\xc0\xaf", "\x80",
                             "\xed\xa0\x80", "\xe0\x80\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xff"};
    for (size_t k = 0; k < sizeof keys / sizeof keys[0]; ++k) {
        for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
            profile_data_t d; profile_ctl_init(&d);
            profile_data_t before = d;
            cfg_changed_t c;
            memset(&c, 0xff, sizeof c);
            char json[160];
            snprintf(json, sizeof json, "{\"battery\":1,\"%s\":\"%s\",\"game_clear\":true}", keys[k], invalid[i]);
            TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, json, (int)strlen(json), &c));
            TEST_ASSERT_EQUAL_MEMORY(&before, &d, sizeof d);
            TEST_ASSERT_FALSE(c.battery || c.name || c.role || c.subtitle || c.avatar_name || c.img_mode || c.game_clear || c.time);
        }
    }
}

TEST_CASE("json truncates valid unicode at existing byte and codepoint limits", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *j = "{\"nickname\":\"😀😀😀😀😀😀😀😀😀😀😀😀\",\"title\":\"😀😀😀😀😀😀😀😀😀😀\",\"intro\":\"123456789012345678901234567890\"}";
    TEST_ASSERT_EQUAL_INT(3, config_json_apply(&d, j, (int)strlen(j), NULL));
    TEST_ASSERT_EQUAL_UINT32(44, strlen(d.name));
    TEST_ASSERT_EQUAL_UINT32(36, strlen(d.role));
    TEST_ASSERT_EQUAL_STRING("1234567890123456789012345678", d.subtitle);
    j = "{\"name\":\"fallback\",\"nickname\":7,\"role\":\"legacy\",\"subtitle\":\"old intro\"}";
    TEST_ASSERT_EQUAL_INT(3, config_json_apply(&d, j, (int)strlen(j), NULL));
    TEST_ASSERT_EQUAL_STRING("fallback", d.name);
    TEST_ASSERT_EQUAL_STRING("legacy", d.role);
    TEST_ASSERT_EQUAL_STRING("old intro", d.subtitle);
}

TEST_CASE("json clamps huge finite values before integer conversion", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    const char *hi = "{\"battery\":1e100,\"level\":1e100,\"xp\":1e100,\"xp_max\":1e100,\"token\":1e100,\"token_max\":1e100,\"sleep_min\":1e100,\"volume\":1e100}";
    TEST_ASSERT_EQUAL_INT(8, config_json_apply(&d, hi, (int)strlen(hi), NULL));
    TEST_ASSERT_EQUAL_INT(100, d.battery);
    TEST_ASSERT_EQUAL_INT(999, d.level);
    TEST_ASSERT_EQUAL_INT(INT_MAX, d.xp);
    TEST_ASSERT_EQUAL_INT(INT_MAX, d.xp_max);
    TEST_ASSERT_EQUAL_INT(1000000000, d.token);
    TEST_ASSERT_EQUAL_INT(1000000000, d.token_max);
    TEST_ASSERT_EQUAL_INT(SLEEP_MIN_MAX, d.sleep_min);
    TEST_ASSERT_EQUAL_INT(100, d.volume);
    const char *lo = "{\"battery\":-1e100,\"level\":-1e100,\"xp\":-1e100,\"xp_max\":-1e100,\"token\":-1e100,\"token_max\":-1e100,\"sleep_min\":-1e100,\"volume\":-1e100}";
    TEST_ASSERT_EQUAL_INT(8, config_json_apply(&d, lo, (int)strlen(lo), NULL));
    TEST_ASSERT_EQUAL_INT(0, d.battery);
    TEST_ASSERT_EQUAL_INT(0, d.level);
    TEST_ASSERT_EQUAL_INT(0, d.xp);
    TEST_ASSERT_EQUAL_INT(1, d.xp_max);
    TEST_ASSERT_EQUAL_INT(0, d.token);
    TEST_ASSERT_EQUAL_INT(1, d.token_max);
    TEST_ASSERT_EQUAL_INT(0, d.sleep_min);
    TEST_ASSERT_EQUAL_INT(0, d.volume);
    const char *fraction = "{\"battery\":82.9,\"xp\":2147483646.9,\"xp_max\":0.9}";
    TEST_ASSERT_EQUAL_INT(3, config_json_apply(&d, fraction, (int)strlen(fraction), NULL));
    TEST_ASSERT_EQUAL_INT(82, d.battery);
    TEST_ASSERT_EQUAL_INT(INT_MAX - 1, d.xp);
    TEST_ASSERT_EQUAL_INT(1, d.xp_max);
}

TEST_CASE("json rejects nonfinite values without partial updates or actions", "[cfgjson]") {
    const char *keys[] = {"battery", "level", "xp", "xp_max", "token", "token_max", "sleep_min", "volume", "time", "game_clear", "unknown"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        for (int sign = 0; sign < 2; ++sign) {
            profile_data_t d; profile_ctl_init(&d);
            profile_data_t before = d;
            cfg_changed_t c;
            char json[160];
            snprintf(json, sizeof json, "{\"nickname\":\"updated\",\"%s\":%s1e1000,\"game_clear\":true}", keys[i], sign ? "-" : "");
            TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, json, (int)strlen(json), &c));
            TEST_ASSERT_EQUAL_MEMORY(&before, &d, sizeof d);
            TEST_ASSERT_FALSE(c.name || c.game_clear || c.time);
        }
    }
}

TEST_CASE("json time is bounded to year 9999 before conversion and RTC use", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    cfg_changed_t c;
    const char *good = "{\"time\":253402300799}";
    TEST_ASSERT_EQUAL_INT(1, config_json_apply(&d, good, (int)strlen(good), &c));
    TEST_ASSERT_TRUE(c.time);
    TEST_ASSERT_EQUAL_INT64(INT64_C(253402300799), c.time_epoch);
    const char *post2038 = "{\"time\":2147483648}";
    TEST_ASSERT_EQUAL_INT(1, config_json_apply(&d, post2038, (int)strlen(post2038), &c));
    TEST_ASSERT_EQUAL_INT64(INT64_C(2147483648), c.time_epoch);
    const char *bad[] = {"{\"time\":253402300800}", "{\"time\":253402300799.5}", "{\"time\":9223372036854775808}", "{\"time\":9223372036854775807}", "{\"time\":1e100}"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, bad[i], (int)strlen(bad[i]), &c));
        TEST_ASSERT_FALSE(c.time);
    }
    const char *ignored = "{\"time\":-1e100}";
    TEST_ASSERT_EQUAL_INT(0, config_json_apply(&d, ignored, (int)strlen(ignored), &c));
}

TEST_CASE("json validates input lengths and rejects trailing payload", "[cfgjson]") {
    profile_data_t d; profile_ctl_init(&d);
    cfg_changed_t c;
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(NULL, "{}", 2, &c));
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, NULL, 2, &c));
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, "{}", -1, &c));
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, "{}", 0, &c));
    TEST_ASSERT_EQUAL_INT(0, config_json_apply(&d, "{}", 2, &c));
    TEST_ASSERT_EQUAL_INT(0, config_json_apply(&d, "{}", 3, &c));
    TEST_ASSERT_EQUAL_INT(0, config_json_apply(&d, "{} \r\n\t", 6, &c));
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, "{} {}", 5, &c));
    TEST_ASSERT_EQUAL_INT(-1, config_json_apply(&d, "{}\0bad", 6, &c));
}
