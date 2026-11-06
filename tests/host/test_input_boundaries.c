/* Small sanitizer probes: keep each case separate so a legacy failure cannot
 * hide the remaining ASan/UBSan regressions. The Unity suite checks semantics. */
#include "services/config_json.h"
#include "services/rtttl.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check_input(const char *data, size_t len) {
    /* Exact allocations let ASan catch even a one-byte read past the supplied
     * non-NUL-terminated input. Failed JSON must never partly update profile. */
    char *exact = malloc(len ? len : 1);
    assert(exact);
    memcpy(exact, data, len);
    profile_data_t d;
    profile_ctl_init(&d);
    profile_data_t before = d;
    if (config_json_apply(&d, exact, (int)len, NULL) < 0)
        assert(memcmp(&before, &d, sizeof d) == 0);
    if (rtttl_validate(exact, (int)len) == 0) {
        rtttl_t r;
        assert(rtttl_init(&r, exact, (int)len) == 0);
        int hz, ms, result;
        size_t notes = 0;
        while ((result = rtttl_next(&r, &hz, &ms)) == 1) {
            assert(hz >= 0 && hz <= 7904);
            assert(ms > 0 && ms <= 14400);
            assert(++notes <= len);
        }
        assert(result == 0 && notes > 0);
    }
    free(exact);
}

static void check_mutations(void) {
    const char *seeds[] = {
        "{\"nickname\":\"名字😀\",\"title\":\"工程师\",\"intro\":\"简介\",\"volume\":85,\"time\":1770000000}",
        "{\"nickname\":\"first\",\"battery\":1e100,\"xp\":-1e100,\"game_clear\":true}",
        "song:d=32,o=6,b=180:b5,4e6,8a.5,8p5."
    };
    size_t cases = 0;
    char input[160];
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; ++i) {
        size_t len = strlen(seeds[i]);
        assert(len < sizeof input);
        memcpy(input, seeds[i], len);
        for (size_t truncated = 0; truncated <= len; ++truncated) {
            check_input(input, truncated);
            ++cases;
        }
        for (size_t p = 0; p < len; ++p) {
            char saved = input[p];
            for (unsigned byte = 0; byte <= 255; ++byte) {
                input[p] = (char)byte;
                check_input(input, len);
                ++cases;
            }
            input[p] = saved;
        }
    }
    uint32_t state = UINT32_C(0x7a13824d);
    for (int i = 0; i < 10000; ++i) {
        size_t len = (unsigned)i % sizeof input;
        for (size_t p = 0; p < len; ++p) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            input[p] = (char)state;
        }
        check_input(input, len);
        ++cases;
    }
    printf("mutations: %zu exact-buffer JSON/RTTTL inputs checked\n", cases);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "mutations")) {
        check_mutations();
        return 0;
    }
    profile_data_t d;
    profile_ctl_init(&d);
    const char *json = NULL;
    const char *score = NULL;
    if (!strcmp(argv[1], "utf8")) json = "{\"intro\":\"\xf0\"}";
    else if (!strcmp(argv[1], "number")) json = "{\"battery\":1e100}";
    else if (!strcmp(argv[1], "time")) json = "{\"time\":1e100}";
    else if (!strcmp(argv[1], "integer")) score = "x:d=999999999999999999999999:oops";
    else if (!strcmp(argv[1], "octave")) score = "x:d=4,o=99,b=120:c";
    else return 2;
    if (json) {
        int result = config_json_apply(&d, json, (int)strlen(json), NULL);
        if (!strcmp(argv[1], "number")) assert(result == 1 && d.battery == 100);
        else assert(result == -1);
        printf("%s: apply=%d\n", argv[1], result);
    } else {
        rtttl_t r;
        int init = rtttl_init(&r, score, (int)strlen(score));
        int hz = 0, ms = 0;
        int next = init == 0 ? rtttl_next(&r, &hz, &ms) : init;
        assert(init == -1);
        printf("%s: init=%d next=%d hz=%d ms=%d\n", argv[1], init, next, hz, ms);
    }
    return 0;
}
