#include "unity.h"
#include "services/rtttl.h"
#include "services/tone_presets.h"
#include <stdlib.h>
#include <string.h>

TEST_CASE("rtttl header defaults + whole_ms", "[rtttl]") {
    rtttl_t r; const char *s = "x:d=8,o=5,b=120:c";
    TEST_ASSERT_EQUAL_INT(0, rtttl_init(&r, s, (int)strlen(s)));
    TEST_ASSERT_EQUAL_INT(8, r.def_dur);
    TEST_ASSERT_EQUAL_INT(5, r.def_oct);
    TEST_ASSERT_EQUAL_INT(120, r.bpm);
    TEST_ASSERT_EQUAL_INT(2000, r.whole_ms);
}
TEST_CASE("rtttl notes freq/ms + rest", "[rtttl]") {
    rtttl_t r; const char *s = "x:d=4,o=5,b=100:8c6,4a,p";
    TEST_ASSERT_EQUAL_INT(0, rtttl_init(&r, s, (int)strlen(s)));
    int f, m;
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &f, &m));
    TEST_ASSERT_EQUAL_INT(1048, f); TEST_ASSERT_EQUAL_INT(300, m);
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &f, &m));
    TEST_ASSERT_EQUAL_INT(880, f);  TEST_ASSERT_EQUAL_INT(600, m);
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &f, &m));
    TEST_ASSERT_EQUAL_INT(0, f);    TEST_ASSERT_EQUAL_INT(600, m);
    TEST_ASSERT_EQUAL_INT(0, rtttl_next(&r, &f, &m));
}
TEST_CASE("rtttl sharp and dotted", "[rtttl]") {
    rtttl_t r; const char *s = "x:d=4,o=5,b=120:c#6,8a.";
    rtttl_init(&r, s, (int)strlen(s));
    int f, m;
    rtttl_next(&r, &f, &m);
    TEST_ASSERT_EQUAL_INT(1108, f);
    TEST_ASSERT_EQUAL_INT(500, m);
    rtttl_next(&r, &f, &m);
    TEST_ASSERT_EQUAL_INT(880, f);
    TEST_ASSERT_EQUAL_INT(375, m);
}

TEST_CASE("rtttl full validation preserves all builtin and client melodies", "[rtttl]") {
    const char *songs[] = {
        TONE_TOKEN_ADD, TONE_TOKEN_SUB, TONE_TOKEN_FAIL, TONE_MARIO_COIN, TONE_FAIL_ALARM, TONE_PASS_OK,
        "Mario:d=4,o=5,b=100:16e6,16e6,32p,16e6,32p,16c6,16e6,16g6,8p,16g,8p",
        "Tetris:d=4,o=5,b=160:e6,8b,8c6,8d6,16e6,16d6,8c6,8b,a,8a,8c6,e6,8d6,8c6,b,8b,8c6,d6,e6,c6,a,2a",
        "Nokia:d=4,o=5,b=125:8e6,8d6,f#,g#,8c#6,8b,d,e,8b,8a,c#,e,2a",
        "defaults::c,C#5,8a.5,8a5.,8P.5,8p5. \r\n"
    };
    for (size_t i = 0; i < sizeof songs / sizeof songs[0]; ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, rtttl_validate(songs[i], (int)strlen(songs[i])), songs[i]);
    }
}

TEST_CASE("rtttl rejects overflowing and out of range header integers", "[rtttl]") {
    const char *bad[] = {
        "x:d=999999999999999999999999:c", "x:o=999999999999999999999999:c",
        "x:b=999999999999999999999999:c", "x:d=0:c", "x:d=3:c", "x:d=64:c",
        "x:o=99:c", "x:o=9:c", "x:o=-1:c", "x:b=0:c", "x:b=24:c", "x:b=901:c",
        "x:d=4", "x:d4:c", "x:q=4:c", "x:d=4,d=8:c", "x:d=4,:c", "x:d=4o=6:c"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        rtttl_t r;
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, rtttl_init(&r, bad[i], (int)strlen(bad[i])), bad[i]);
        TEST_ASSERT_EQUAL_INT(-1, rtttl_validate(bad[i], (int)strlen(bad[i])));
    }
}

TEST_CASE("rtttl validates every note and rejects malformed suffixes", "[rtttl]") {
    const char *bad[] = {
        "x::c,8z", "x::c,4c99", "x::c,4p99", "x::c,99999999999999999999999c",
        "x::c,c99999999999999999999999", "x::c,p99999999999999999999999",
        "x::c,0c", "x::c,3c", "x::c,64c", "x::c,c..", "x::c,c.5.", "x::c,c5.5",
        "x::c,c-1", "x::c,c9", "x::c,b#8", "x::c,c##", "x::c,p#", "x::cd",
        "x::c d", "x::c,,d", "x::c,", "x::,c", "x::c trailing", "x::c:bad", "x::", "x:: \r\n"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, rtttl_validate(bad[i], (int)strlen(bad[i])), bad[i]);
    }
}

TEST_CASE("rtttl valid numeric boundaries never overflow shifts or durations", "[rtttl]") {
    const char *song = "x:d=1,o=0,b=25:c,32b8,1b#7.,p0";
    rtttl_t r;
    int hz, ms;
    TEST_ASSERT_EQUAL_INT(0, rtttl_validate(song, (int)strlen(song)));
    TEST_ASSERT_EQUAL_INT(0, rtttl_init(&r, song, (int)strlen(song)));
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(16, hz);
    TEST_ASSERT_EQUAL_INT(9600, ms);
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(7904, hz);
    TEST_ASSERT_EQUAL_INT(300, ms);
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(4192, hz);
    TEST_ASSERT_EQUAL_INT(14400, ms);
    TEST_ASSERT_EQUAL_INT(1, rtttl_next(&r, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(0, hz);
    TEST_ASSERT_EQUAL_INT(9600, ms);
    TEST_ASSERT_EQUAL_INT(0, rtttl_next(&r, &hz, &ms));
    song = "x:d=32,o=8,b=900:c";
    TEST_ASSERT_EQUAL_INT(0, rtttl_validate(song, (int)strlen(song)));
}

TEST_CASE("rtttl handles exact bounded buffers and invalid API arguments", "[rtttl]") {
    const char *song = "x::c";
    size_t len = strlen(song);
    char *exact = malloc(len);
    TEST_ASSERT_NOT_NULL(exact);
    memcpy(exact, song, len);
    int result = rtttl_validate(exact, (int)len);
    free(exact);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_INT(0, rtttl_validate(song, (int)len + 1));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_validate("x::c\0,invalid", 13));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_validate(NULL, 1));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_validate(song, -1));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_validate(song, 0));
    rtttl_t r = {0};
    int hz = 123, ms = 456;
    TEST_ASSERT_EQUAL_INT(-1, rtttl_init(NULL, song, (int)len));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_next(NULL, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_next(&r, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(0, rtttl_init(&r, "x::c99", 6));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_next(&r, &hz, &ms));
    TEST_ASSERT_EQUAL_INT(123, hz);
    TEST_ASSERT_EQUAL_INT(456, ms);
    TEST_ASSERT_EQUAL_INT(-1, rtttl_next(&r, NULL, &ms));
    TEST_ASSERT_EQUAL_INT(-1, rtttl_next(&r, &hz, NULL));
}
