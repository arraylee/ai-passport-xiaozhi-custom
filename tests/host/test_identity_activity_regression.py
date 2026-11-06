"""Execute real app identity/activity functions with real broadcast parsers.

Only hardware/persistence/UI boundaries and the injected MAC calculator are
substituted. The deterministic MAC covers all signed bytes, like core Unity
tests; this is not a claim about cryptographic strength or on-device timing.
"""
import os
from pathlib import Path
import tempfile
import unittest

from test_ble_advertising import function
from test_clicker_platform import host_sanitizers, run_host

ROOT = Path(__file__).resolve().parents[2]
SERVICES = ROOT / "components/core/services"

BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include "hal/hal_config.h"
#include "hal/hal_identity.h"
#include "services/ble_match.h"
#include "services/profile_ctl.h"
#include "services/token_bcast.h"
#include "services/profile_bcast.h"
#include "services/profile_batch_bcast.h"
#include "services/tone_presets.h"
#define PERIPH_AUDIO 0
#define PERIPH_DISPLAY 0
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define IDENT_BLOB_SIZE 72
static uint8_t s_ident_blob[IDENT_BLOB_SIZE], emitted[IDENT_BLOB_SIZE];
static unsigned identity_calls, key_calls, activity_calls, saves;
static bool provisioned, sn_missing, save_ok = true, avatar_available = true;
static bool guide_active;
static bool ui_button_guide_active(void) { return guide_active; }
static int64_t clock_us = 10000000;
static atomic_bool s_sleep_preparing;
static profile_data_t s_profile;
static token_bcast_t s_tb;
static profile_bcast_t s_pb;
static profile_batch_bcast_t s_pbb;
static const uint8_t SELF[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
static int64_t esp_timer_get_time(void) { return clock_us; }
static void note_activity(void) { ++activity_calls; }
static bool save_profile(void) { ++saves; return save_ok; }
static bool avatar_store_has(const char *name) { (void)name; return avatar_available; }
static int avatar_store_name_at(uint8_t index, char name[16]) {
    (void)index;
    if (!avatar_available) return -1;
    strcpy(name, "default"); return 0;
}
static int test_settimeofday(const struct timeval *tv, const void *tz) {
    (void)tv; (void)tz; return 0;
}
#define settimeofday test_settimeofday
static int identity_sn(hal_identity_t *self, char *out, int cap) {
    (void)self; assert(cap >= HAL_IDENTITY_SN_CAP);
    if (sn_missing) return -1;
    strcpy(out, "112233445566"); return 12;
}
static int identity_key(hal_identity_t *self, char *out, int cap) {
    (void)self; (void)out; (void)cap;
    ++key_calls; assert(!"publish_identity must never read DeviceSecret"); return -1;
}
static int identity_hw(hal_identity_t *self, char *out, int cap) {
    (void)self; assert(cap >= HAL_IDENTITY_HW_CAP);
    strcpy(out, "revision12345678"); return 16;
}
static bool identity_provisioned(hal_identity_t *self) { (void)self; return provisioned; }
static void identity_set(hal_config_t *self, const uint8_t *data, int len) {
    (void)self; assert(len == IDENT_BLOB_SIZE);
    memcpy(emitted, data, sizeof emitted); ++identity_calls;
}
static const hal_identity_api_t id_api = {
    .get_sn = identity_sn, .get_key = identity_key, .get_hw_ver = identity_hw,
    .is_provisioned = identity_provisioned,
};
static const hal_config_api_t cfg_api = {.set_identity = identity_set};
static hal_identity_t identity = {.api = &id_api};
static hal_config_t config = {.api = &cfg_api};
static hal_identity_t *s_id = &identity;
static hal_config_t *s_cfg = &config;
'''

CASES = r'''
static void test_identity(void) {
    for (int active = 0; active < 2; ++active) {
        provisioned = active;
        memset(s_ident_blob, 0xa5, sizeof s_ident_blob);
        publish_identity();
        uint8_t expected[IDENT_BLOB_SIZE] = {2, (uint8_t)(2 | active)};
        memcpy(expected + 2, "112233445566", 12);
        memcpy(expected + 58, "revision", 8);
        assert(memcmp(expected, emitted, sizeof expected) == 0);
        assert(key_calls == 0);
    }
    assert(identity_calls == 2);
    sn_missing = true; publish_identity();
    for (int i = 2; i < 58; ++i) assert(emitted[i] == 0);
    s_cfg = NULL; publish_identity(); assert(identity_calls == 3);
    s_cfg = &config; s_id = NULL; publish_identity(); assert(identity_calls == 3);
    s_id = &identity; assert(key_calls == 0);
}
static void send(const uint8_t *data, int len, unsigned expected_activity) {
    unsigned before = activity_calls;
    clock_us += 10000;
    on_ble_match(data, len, NULL);
    assert(activity_calls == before + expected_activity);
}
static void test_activity(void) {
    profile_ctl_init(&s_profile);
    token_bcast_init(&s_tb, SELF, fake_mac, NULL);
    profile_bcast_init(&s_pb, SELF, fake_mac, NULL);
    profile_batch_bcast_init(&s_pbb, fake_mac, NULL);
    uint8_t f[26];
    const uint8_t heartbeat[] = {0xff, 0xff, 'H', 'B'};
    send(heartbeat, sizeof heartbeat, 0);
    send(NULL, 0, 0);
    build_frame(f, SELF, 1, 100, 1); f[18] ^= 1; send(f, 26, 0); // bad MAC
    send(f, 26, 0); // rate-limited bad MAC
    build_frame(f, SELF, 1, 100, 1); send(f, 25, 0); // coarse-match bad length
    f[6] ^= 1; fake_mac(f, 18, f + 18, NULL); send(f, 26, 0); // other badge
    build_frame(f, SELF, 1, 100, 1); send(f, 26, 1);
    assert(s_profile.token == 100);
    send(f, 26, 0); // authenticated replay
    build_frame(f, SELF, 2, 999, 9); send(f, 26, 0); // signed invalid op
    build_frame(f, SELF, 3, 1000000, 1); send(f, 26, 0); // signed invalid balance
    build_frame(f, SELF, 4, 300, 4); send(f, 26, 1); // sync
    build_frame(f, SELF, 5, 200, 3); send(f, 26, 1); // signed business failure
    profile_data_t before_pet = s_profile;
    unsigned saves_before_pet = saves;
    build_frame(f, SELF, 6, 1, 5); send(f, 26, 0); // removed pet operation
    assert(memcmp(&s_profile, &before_pet, sizeof s_profile) == 0);
    assert(saves == saves_before_pet);
    s_tb.enabled = false;
    build_frame(f, SELF, 7, 200, 1); send(f, 26, 0);
    s_tb.enabled = true;
    atomic_store(&s_sleep_preparing, true); send(f, 26, 0);
    atomic_store(&s_sleep_preparing, false); send(f, 26, 1);

    const uint8_t head[3] = {'a', 'b', 'c'}, tail[3] = {'d', 0, 0};
    frame(f, PB_FIELD_NICKNAME, 10, 0, 2, 4, head);
    f[18] ^= 1; send(f, 26, 0); // error
    f[18] ^= 1; send(f, 26, 0); // incomplete
    send(f, 26, 0); // duplicate fragment
    frame(f, PB_FIELD_NICKNAME, 10, 1, 2, 4, tail); send(f, 26, 1);
    assert(strcmp(s_profile.name, "abcd") == 0);
    send(f, 26, 0); // completed transaction replay
    const uint8_t short_name[3] = {'n', 0, 0};
    frame(f, PB_FIELD_NICKNAME, 11, 0, 1, 1, short_name);
    save_ok = false; send(f, 26, 0); save_ok = true; // storage failure
    avatar_available = false;
    frame(f, PB_FIELD_AVATAR_NAME, 12, 0, 1, 1, short_name); send(f, 26, 0);
    avatar_available = true;

    common(f, 20, PBB_FIELD_TOKEN, 500, 0, 0, 0);
    f[18] ^= 1; send(f, 26, 0); f[18] ^= 1;
    send(f, 26, 1); assert(s_profile.token == 500);
    send(f, 26, 0); // common replay
    common(f, 21, PBB_FIELD_TOKEN_MAX, 0, 0, 0, 0); send(f, 26, 0); // error
    const uint8_t nickname[] = "abcdefghijkl";
    nick(f, 22, 0, 2, 12, nickname, 11); send(f, 26, 0); send(f, 26, 0);
    nick(f, 22, 1, 2, 12, nickname + 11, 1); send(f, 26, 1);
    assert(strcmp(s_profile.name, (const char *)nickname) == 0);
    send(f, 26, 0);
    common(f, 23, PBB_FIELD_TOKEN, 700, 0, 0, 0);
    save_ok = false; send(f, 26, 0); assert(s_profile.token == 500);
    save_ok = true; send(f, 26, 1); assert(s_profile.token == 700);
    assert(saves > 0);
}
int main(void) {
    fputs("PASSPORT_IDENTITY_MAIN_ENTERED\nPASSPORT_IDENTITY_CASE activity\n", stderr); fflush(stderr);
    test_identity(); test_activity();
    guide_active = false; assert(capture_screen_allowed());
    guide_active = true; assert(!capture_screen_allowed());
    puts("identity v2 and real token/profile/batch activity regressions: OK");
    return 0;
}
'''

BOOT_BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "hal/hal_identity.h"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
static atomic_bool s_config_connected, s_boot_avatar_ready;
static bool s_button_guide_active, s_button_guide_exit, guide_open;
static bool sn_missing, key_missing, fail_lock, fail_open, nav_enabled = true;
static unsigned restore_calls, open_calls, waits, key_caps;
static int *s_button;
static int get_sn(hal_identity_t *self, char *out, int cap) {
    (void)self; assert(cap == HAL_IDENTITY_SN_CAP);
    if (sn_missing) return -1;
    strcpy(out, "112233445566"); return 12;
}
static int get_key(hal_identity_t *self, char *out, int cap) {
    (void)self; key_caps = (unsigned)cap; assert(cap == HAL_IDENTITY_KEY_CAP);
    if (key_missing) return -1;
    memset(out, 'x', 64); out[64] = 0; return 64;
}
static const hal_identity_api_t api = {.get_sn = get_sn, .get_key = get_key};
static hal_identity_t id = {.api = &api};
static hal_identity_t *s_id = &id;
static void restore_boot_avatar(void) { ++restore_calls; atomic_store(&s_boot_avatar_ready, true); }
static bool platform_lvgl_lock(int timeout) { (void)timeout; return !fail_lock; }
static void platform_lvgl_unlock(void) {}
static void platform_lvgl_nav_enable(bool enabled) { nav_enabled = enabled; }
static void ui_button_guide_open(const char *sn, const char *key) {
    assert(strcmp(sn, "112233445566") == 0); assert(strlen(key) == 64);
    for (int i = 0; i < 64; ++i) assert(key[i] == 'x');
    ++open_calls; guide_open = !fail_open;
}
static bool ui_button_guide_active(void) { return guide_open; }
static void ui_button_guide_close(void) { guide_open = false; }
static bool platform_button_any_pressed(void *button) { (void)button; return false; }
static void vTaskDelay(int ticks) { (void)ticks; ++waits; s_button_guide_exit = true; }
static void note_activity(void) {}
'''

BOOT_CASES = r'''
int main(void) {
    fputs("PASSPORT_IDENTITY_MAIN_ENTERED\nPASSPORT_IDENTITY_CASE boot\n", stderr); fflush(stderr);
    sn_missing = true; show_boot_button_guide();
    assert(restore_calls == 1 && open_calls == 0 && waits == 0 && nav_enabled);
    assert(!s_button_guide_active && key_caps == 0);
    sn_missing = false; key_missing = true; show_boot_button_guide();
    assert(restore_calls == 2 && open_calls == 0 && waits == 0 && nav_enabled);
    assert(key_caps == 65 && !s_button_guide_active);
    key_missing = false; fail_lock = true; show_boot_button_guide();
    assert(restore_calls == 3 && open_calls == 0 && waits == 0 && nav_enabled);
    fail_lock = false; atomic_store(&s_config_connected, true); show_boot_button_guide();
    assert(restore_calls == 4 && open_calls == 0 && waits == 0 && nav_enabled);
    atomic_store(&s_config_connected, false); fail_open = true; show_boot_button_guide();
    assert(restore_calls == 5 && open_calls == 1 && waits == 0 && nav_enabled);
    assert(!s_button_guide_active);
    fail_open = false; show_boot_button_guide();
    assert(restore_calls == 6 && open_calls == 2 && waits > 0 && nav_enabled);
    assert(!guide_open && !s_button_guide_active && key_caps == 65);
    return 0;
}
'''


class IdentityActivityRegression(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sanitizers = host_sanitizers()
        print(f"Identity host sanitizers={cls.sanitizers}" +
              (" (explicit opt-in; AddressSanitizer coverage disabled)"
               if cls.sanitizers == "undefined" else " (default ASAN + UBSan)"))

    def run_fixture(self, executable, phase):
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        run_host([str(executable)], timeout=5, phase=phase, sanitizers=self.sanitizers,
                 check_main=True, env=environment, main_marker="PASSPORT_IDENTITY_MAIN_ENTERED",
                 case_marker="PASSPORT_IDENTITY_CASE ")

    def test_boot_missing_identity_restores_avatar_without_freezing_navigation(self):
        app = (ROOT / "components/app/src/app.c").read_text()
        with tempfile.TemporaryDirectory(prefix="passport-boot-identity-", dir="/tmp") as directory:
            source = Path(directory) / "test.c"
            executable = Path(directory) / "test"
            source.write_text(BOOT_BOUNDARY + function(app, "show_boot_button_guide") + BOOT_CASES)
            run_host([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-g",
                f"-fsanitize={self.sanitizers}", "-fno-sanitize-recover=all",
                "-I" + str(ROOT / "components/core/ports/include"),
                str(source), "-o", str(executable),
            ], timeout=30, phase="identity fixture compile", sanitizers=self.sanitizers)
            self.run_fixture(executable, "identity fixture")

    def test_real_app_identity_and_broadcast_activity(self):
        app = (ROOT / "components/app/src/app.c").read_text()
        token_test = (SERVICES / "test/test_token_bcast.c").read_text()
        profile_test = (SERVICES / "test/test_profile_bcast.c").read_text()
        batch_test = (SERVICES / "test/test_profile_batch_bcast.c").read_text()
        helpers = "\n".join([
            *(function(token_test, name) for name in ("fake_mac", "wr_u24", "build_frame")),
            function(profile_test, "frame"),
            *(function(batch_test, name) for name in ("sign_frame", "common", "nick")),
        ])
        app_functions = "\n".join(function(app, name) for name in (
            "publish_identity", "on_token_bcast", "read_u32_le", "on_profile_bcast",
            "on_profile_batch_bcast", "on_ble_match", "capture_screen_allowed",
        ))
        with tempfile.TemporaryDirectory(prefix="passport-identity-", dir="/tmp") as directory:
            source = Path(directory) / "test.c"
            executable = Path(directory) / "test"
            source.write_text(BOUNDARY + helpers + app_functions + CASES)
            production = [SERVICES / "src" / name for name in (
                "ble_match.c", "token_bcast.c", "profile_bcast.c", "profile_batch_bcast.c", "profile_ctl.c",
            )]
            run_host([
                "cc", "-std=c11", "-D_DEFAULT_SOURCE", "-Wall", "-Werror",
                "-Wno-unused-variable", "-Wno-unused-but-set-variable",
                f"-fsanitize={self.sanitizers}", "-fno-sanitize-recover=all", "-g",
                "-I" + str(SERVICES / "include"), "-I" + str(ROOT / "components/core/ports/include"),
                str(source), *map(str, production), "-o", str(executable),
            ], timeout=30, phase="identity fixture compile", sanitizers=self.sanitizers)
            self.run_fixture(executable, "identity fixture")

    def test_full_score_validation_at_each_untrusted_boundary(self):
        app = (ROOT / "components/app/src/app.c").read_text()
        for name in ("has_user_configured_content", "play_saved_boot_rtttl", "on_cfg_message"):
            self.assertIn("rtttl_validate(", function(app, name), name)
        receive = function(app, "on_cfg_message")
        self.assertLess(receive.index("rtttl_validate("), receive.index("hal_kv_set(s_kv, BOOT_RTTTL_KV_KEY"))
        boot = function(app, "play_saved_boot_rtttl")
        self.assertLess(boot.index("rtttl_validate("), boot.index("xSemaphoreGive("))
        self.assertIn("key[HAL_IDENTITY_KEY_CAP]", function(app, "show_boot_button_guide"))

    def test_capture_policy_is_checked_inside_the_redraw_lock(self):
        app = (ROOT / "components/app/src/app.c").read_text()
        platform = (ROOT / "components/platform/platform_esp32/src/screen_capture_lvgl.c").read_text()
        start = function(platform, "platform_screen_capture_start")
        self.assertLess(start.index("platform_lvgl_lock("), start.index("!s_allowed || !s_allowed()"))
        self.assertLess(start.index("!s_allowed || !s_allowed()"), start.index("lv_refr_now("))
        self.assertIn("platform_screen_capture_set_policy(capture_screen_allowed)",
                      function(app, "capture_orchestration_init"))


if __name__ == "__main__":
    unittest.main()
