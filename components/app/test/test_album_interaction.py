"""Execute the production ALBUM handlers and viewer state machine with host stubs.

Only hardware/UI boundary calls are stubbed. The C functions under test are
extracted verbatim, compiled by cc, and exercised with behavioral assertions.
All generated sources and executables live in a temporary directory.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
APP_SOURCE = ROOT / "components/app/src/app.c"
VIEW_SOURCE = ROOT / "components/platform/platform_esp32/src/jpeg_view.c"


def extract_function(source: str, name: str) -> str:
    """Find a complete definition without counting braces in comments/literals."""
    literals = re.compile(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        re.DOTALL,
    )
    masked = literals.sub(
        lambda match: "".join("\n" if ch == "\n" else " " for ch in match[0]),
        source,
    )
    declaration = re.search(
        rf"(?m)^[ \t]*(?:static\s+)?(?:void|bool)\s+{re.escape(name)}"
        r"\s*\([^;{}]*\)\s*\{",
        masked,
    )
    if declaration is None:
        raise AssertionError(f"production function {name} is missing")
    depth = 0
    for index in range(masked.index("{", declaration.start()), len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return source[declaration.start():index + 1]
    raise AssertionError(f"production function {name} has no closing brace")


COMMON = r"""
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef enum { JPEG_VIEW_FULLSCREEN = 0, JPEG_VIEW_AVATAR = 1 } jpeg_view_mode_t;
"""


APP_STUBS = r"""
#include "hal/hal_button.h"
#include "hal/hal_config.h"
#include "presentation/ui_game.h"
#include "presentation/ui_album.h"
#include "services/jpeg_probe.h"
#define PERIPH_BLE 1
#define PERIPH_DISPLAY 1
#define PERIPH_BUTTON 1
#define VIEW_REENTER_GUARD_MS 1000u
typedef enum { JPEG_SLOT_FULL = 0, JPEG_SLOT_AVATAR = 1 } jpeg_slot_t;
static uint32_t s_view_exit_ms;
static bool s_view_nav_restore_pending, s_button_guide_active, s_button_guide_exit;
static bool s_factory_mode, s_btn_test_active;
static atomic_bool s_factory_command_active, s_deep_sleep_entering, s_sleep_preparing;
static uint32_t s_last_activity_ms;
static int s_power_guard;
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static uint8_t s_btn_test_count[3];
static int s_btn_test_mask;
static struct { int game_best, pet_type; } s_profile;

static bool has_full, fullscreen, viewer_active, game_active, pet_active;
static bool nav_enabled = true, request_saw_nav_disabled;
static int prompt_calls, nav_calls, request_calls, requested_mode = -1;
static int exit_calls, game_key_calls, game_open_calls, pet_open_calls, pet_close_calls;
static int status_calls;
static unsigned album_key_calls;
static ui_album_action_t album_action;
static uint8_t last_status_type, last_status;
static int64_t now_us = 45678987;

static bool capture_ui_blocked(void) { return false; }
static bool app_plays_active(void) { return false; }
static bool app_plays_open(int best) { (void)best; game_open_calls++; return true; }
static void app_plays_button(int i, hal_btn_event_t e) { (void)i; (void)e; }
static void app_plays_cancel_sleep(void) {}
bool ui_album_active(void) { return false; }
void ui_album_no_photo(void *parent) { (void)parent; prompt_calls++; }
ui_album_action_t ui_album_button(int index, ui_album_event_t event) {
    (void)event; if(index < 0 || index >= 3) return UI_ALBUM_NONE;
    album_key_calls++; return album_action;
}
static unsigned license_calls;
static bool ui_profile_open_licenses(void) { ++license_calls; return true; }
static void note_activity(void) {}
static int64_t esp_timer_get_time(void) { return now_us; }
static void on_game_exit(void *user) { (void)user; }
static void on_pet_exit(void *user) { (void)user; }
void ui_game_set_best(int best) { (void)best; }
void ui_game_open(ui_game_exit_cb_t cb, void *user) {
    (void)cb; (void)user; game_open_calls++;
}
bool ui_game_is_active(void) { return game_active; }
void ui_game_key(ui_game_key_t key) { (void)key; game_key_calls++; }
static void platform_lvgl_nav_enable(bool enabled) { nav_enabled = enabled; nav_calls++; }
static bool jpeg_store_has_valid(jpeg_slot_t slot) { CHECK(slot == JPEG_SLOT_FULL); return has_full; }
static void jpeg_view_request_mode(jpeg_view_mode_t mode) {
    request_calls++; requested_mode = mode; request_saw_nav_disabled = !nav_enabled;
}
static void ui_profile_show_album_empty(void) { prompt_calls++; }
static void ui_pet_open(int type, bool reveal, void (*cb)(void *), void *user) {
    (void)type; (void)reveal; (void)cb; (void)user; pet_open_calls++;
}
static bool ui_pet_is_active(void) { return pet_active; }
static bool platform_lvgl_lock(int timeout) { (void)timeout; return true; }
static void platform_lvgl_unlock(void) {}
static void ui_pet_close(void) { pet_close_calls++; }
static bool jpeg_view_is_active(void) { return viewer_active; }
static bool jpeg_view_is_fullscreen(void) { return fullscreen; }
static void jpeg_view_exit(void) { exit_calls++; }
static jpeg_probe_t jpeg_view_last_probe(void) { return JPEG_PROBE_OK; }
static void notify_status(uint8_t type, uint8_t status) {
    status_calls++; last_status_type = type; last_status = status;
}
"""


APP_CASES = r"""
static void check_no_exit(void) {
    CHECK(exit_calls == 0);
    CHECK(!s_view_nav_restore_pending);
    CHECK(s_view_exit_ms == 0);
    CHECK(game_key_calls == 0);
    CHECK(nav_calls == 0);
}

int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (strcmp(argv[1], "home_hold") == 0) {
        for (int index=0; index<3; ++index) {
            on_btn_raw(index, HAL_BTN_PRESS, NULL);
            on_btn_raw(index, HAL_BTN_LONG, NULL);
            on_btn_raw(index, HAL_BTN_CLICK, NULL);
        }
        CHECK(license_calls==0 && game_key_calls==0 && nav_calls==0);
    } else if (strcmp(argv[1], "empty") == 0) {
        /* A first ALBUM click also works during the first second after boot. */
        now_us = 100000;
        on_dock_action(3, NULL);
        CHECK(prompt_calls == 1);
        CHECK(request_calls == 0);
        CHECK(nav_calls == 1 && !nav_enabled); // User-requested modal no-photo message.
    } else if (strcmp(argv[1], "saved") == 0) {
        has_full = true;
        on_dock_action(3, NULL);
        CHECK(prompt_calls == 0);
        CHECK(request_calls == 1 && requested_mode == JPEG_VIEW_FULLSCREEN);
        CHECK(nav_calls == 1 && !nav_enabled);
        CHECK(request_saw_nav_disabled);
    } else if (strcmp(argv[1], "press") == 0) {
        fullscreen = viewer_active = game_active = pet_active = true;
        for (int index = 0; index < 3; index++) {
            exit_calls = 0;
            s_view_exit_ms = 0;
            s_view_nav_restore_pending = false;
            on_btn_raw(index, HAL_BTN_PRESS, NULL);
            CHECK(exit_calls == 0); // Press opens/operates confirmation; not an immediate exit.
            CHECK(!s_view_nav_restore_pending);
            CHECK(album_key_calls == (unsigned)index + 1);
            CHECK(game_key_calls == 0 && nav_calls == 0);
            CHECK(pet_close_calls == 0);
        }
    } else if (strcmp(argv[1], "ignored") == 0) {
        fullscreen = viewer_active = game_active = true;
        const hal_btn_event_t events[] = { HAL_BTN_DOUBLE };
        for (int index = 0; index < 3; index++) {
            for (size_t event = 0; event < sizeof events / sizeof events[0]; event++) {
                on_btn_raw(index, events[event], NULL);
                check_no_exit();
            }
        }
        const int invalid_indices[] = { -1, 3, 255 };
        for (size_t index = 0; index < sizeof invalid_indices / sizeof invalid_indices[0]; index++) {
            on_btn_raw(invalid_indices[index], HAL_BTN_PRESS, NULL);
            check_no_exit();
        }
    } else if (strcmp(argv[1], "pending") == 0) {
        s_view_nav_restore_pending = true;
        s_view_exit_ms = 1234;
        game_active = has_full = true;
        const hal_btn_event_t events[] = {
            HAL_BTN_PRESS, HAL_BTN_CLICK, HAL_BTN_DOUBLE, HAL_BTN_LONG
        };
        for (int active = 0; active < 2; active++) {
            fullscreen = viewer_active = (active != 0);
            for (int index = 0; index < 3; index++) {
                for (size_t event = 0; event < sizeof events / sizeof events[0]; event++)
                    on_btn_raw(index, events[event], NULL);
            }
        }
        /* Even if another page becomes active, release events stay swallowed. */
        pet_active = true;
        on_btn_raw(2, HAL_BTN_LONG, NULL);
        for (int id = 2; id <= 4; id++) on_dock_action(id, NULL);
        CHECK(exit_calls == 0 && game_key_calls == 0 && pet_close_calls == 0);
        CHECK(request_calls == 0 && prompt_calls == 0 && nav_calls == 0);
        CHECK(game_open_calls == 0 && pet_open_calls == 0);
        CHECK(s_view_nav_restore_pending && s_view_exit_ms == 1234);
    } else if (strcmp(argv[1], "avatar") == 0) {
        viewer_active = true;
        fullscreen = false;
        const hal_btn_event_t events[] = {
            HAL_BTN_PRESS, HAL_BTN_CLICK, HAL_BTN_DOUBLE, HAL_BTN_LONG
        };
        for (int index = 0; index < 3; index++) {
            for (size_t event = 0; event < sizeof events / sizeof events[0]; event++) {
                on_btn_raw(index, events[event], NULL);
                check_no_exit();
            }
        }
    } else if (strcmp(argv[1], "success_pending") == 0) {
        fullscreen = viewer_active = s_view_nav_restore_pending = true;
        nav_enabled = false;
        on_jpeg_result(1, NULL);
        CHECK(exit_calls == 1);
        CHECK(s_view_nav_restore_pending);
        CHECK(nav_calls == 0 && !nav_enabled);
        CHECK(status_calls == 1 && last_status_type == CFG_MSG_JPEG);
        CHECK(last_status == CFG_ST_DONE);
    } else if (strcmp(argv[1], "failure_pending") == 0) {
        s_view_nav_restore_pending = true;
        nav_enabled = false;
        for (int active = 0; active < 2; active++) {
            fullscreen = viewer_active = (active != 0);
            on_jpeg_result(0, NULL);
            CHECK(nav_calls == 0 && !nav_enabled);
            CHECK(s_view_nav_restore_pending && exit_calls == 0);
            CHECK(status_calls == active + 1 && last_status_type == CFG_MSG_JPEG);
            CHECK(last_status == CFG_ST_ERR_DECODE);
        }
    } else if (strcmp(argv[1], "ordinary_failure") == 0) {
        nav_enabled = false;
        on_jpeg_result(0, NULL);
        CHECK(nav_calls == 0 && !nav_enabled); // Actual idle callback restores only after avatar completion.
        CHECK(s_view_nav_restore_pending && exit_calls == 0);
        CHECK(status_calls == 1 && last_status_type == CFG_MSG_JPEG);
        CHECK(last_status == CFG_ST_ERR_DECODE);
    } else if (strcmp(argv[1], "confirm") == 0) {
        fullscreen=viewer_active=true;
        album_action=UI_ALBUM_RETURN_HOME;
        on_btn_raw(2, HAL_BTN_CLICK, NULL);
        CHECK(exit_calls==1 && s_view_nav_restore_pending && nav_calls==0);
        CHECK(s_view_exit_ms==(uint32_t)(now_us/1000));
        on_btn_raw(2, HAL_BTN_CLICK, NULL); CHECK(exit_calls==1);
    } else {
        CHECK(false);
    }
    return 0;
}
"""


VIEW_STUBS = r"""
typedef struct { int unused; } lv_timer_t;
typedef void (*jpeg_view_result_cb_t)(int ok, void *user);
typedef void (*jpeg_view_avatar_cb_t)(const void *dsc, void *user);
static volatile int s_pending, s_full_pending, s_exit_req, s_restore_only;
static bool s_active;
static volatile bool s_decoding;
static int s_req_mode, s_dsc;
static char s_avatar_name[16] = "default", s_avatar_pending[16] = "default";
static bool s_avatar_named = true, s_avatar_named_pending = true;
static jpeg_view_result_cb_t s_result_cb;
static jpeg_view_avatar_cb_t s_avatar_cb;
static void *s_result_user, *s_avatar_user;
static bool (*s_can_show_cb)(void *);
static void *s_can_show_user;
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
static int show_calls, teardown_calls, result_calls;
static bool exit_during_decode;
void jpeg_view_exit(void);
bool jpeg_view_is_fullscreen(void);
static int build_dsc(void) { return 0; }
static int show_image(void) {
    show_calls++;
    s_decoding = true;
    CHECK(jpeg_view_is_fullscreen());
    if (exit_during_decode) jpeg_view_exit();
    s_decoding = false;
    return 0;
}
/* Rendering and avatar restoration are external boundaries to tick here. */
static void teardown(void) { teardown_calls++; s_active = false; }
static void record_result(int ok, void *user) {
    (void)user; CHECK(ok == 1); result_calls++;
}
"""


VIEW_CASES = r"""
int main(int argc, char **argv) {
    CHECK(argc == 2);
    s_result_cb = record_result;
    if (strcmp(argv[1], "states") == 0) {
        CHECK(!jpeg_view_is_active() && !jpeg_view_is_fullscreen());
        jpeg_view_request_mode(JPEG_VIEW_AVATAR);
        CHECK(jpeg_view_is_active() && !jpeg_view_is_fullscreen());
        s_pending = 0;
        s_req_mode = JPEG_VIEW_AVATAR; // Set by restore_pending_avatar before decode.
        s_decoding = true;
        CHECK(!jpeg_view_is_fullscreen());
        s_decoding = false;
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        CHECK(jpeg_view_is_active() && jpeg_view_is_fullscreen());
        s_full_pending = 0;
        s_req_mode = JPEG_VIEW_FULLSCREEN; // Set by tick before show_image.
        s_decoding = true;
        CHECK(jpeg_view_is_fullscreen());
        s_decoding = false;
        CHECK(!jpeg_view_is_fullscreen());
        s_active = true;
        s_req_mode = JPEG_VIEW_AVATAR;
        CHECK(jpeg_view_is_fullscreen());
    } else if (strcmp(argv[1], "queued_exit") == 0) {
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        jpeg_view_exit();
        tick(NULL);
        CHECK(show_calls == 1 && teardown_calls == 1);
        CHECK(!jpeg_view_is_fullscreen() && !jpeg_view_is_active());
        CHECK(result_calls == 1);
    } else if (strcmp(argv[1], "decode_exit") == 0) {
        exit_during_decode = true;
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        tick(NULL);
        CHECK(show_calls == 1 && teardown_calls == 1);
        CHECK(!jpeg_view_is_fullscreen() && !jpeg_view_is_active());
        CHECK(result_calls == 1);
    } else if (strcmp(argv[1], "fresh_request") == 0) {
        jpeg_view_exit();
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        tick(NULL);
        CHECK(show_calls == 1 && teardown_calls == 0);
        CHECK(jpeg_view_is_fullscreen());
        CHECK(result_calls == 1);
    } else if (strcmp(argv[1], "replace_queued") == 0 ||
               strcmp(argv[1], "replace_active") == 0) {
        bool was_active = strcmp(argv[1], "replace_active") == 0;
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        if (was_active) tick(NULL);
        jpeg_view_exit();
        /* A replacement upload must not cancel the user's outstanding exit. */
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        tick(NULL);
        CHECK(teardown_calls == 1);
        CHECK(!jpeg_view_is_fullscreen() && !jpeg_view_is_active());
        CHECK(show_calls == (was_active ? 2 : 1));
        CHECK(result_calls == show_calls);
    } else if (strcmp(argv[1], "resident") == 0) {
        jpeg_view_request_mode(JPEG_VIEW_FULLSCREEN);
        tick(NULL);
        for (int n = 0; n < 1000; n++) tick(NULL);
        CHECK(jpeg_view_is_fullscreen() && teardown_calls == 0);
        CHECK(show_calls == 1 && result_calls == 1);
        jpeg_view_exit();
        tick(NULL);
        CHECK(!jpeg_view_is_fullscreen() && teardown_calls == 1);
        CHECK(result_calls == 1); /* No extra image-result callback for exit. */
        tick(NULL);
        CHECK(teardown_calls == 1 && result_calls == 1);
    } else {
        CHECK(false);
    }
    return 0;
}
"""


class AlbumInteractionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("host C compiler cc is required")
        temporary = tempfile.TemporaryDirectory(prefix="album-interaction-")
        cls.addClassCleanup(temporary.cleanup)
        cls.workdir = Path(temporary.name)
        cls.executables = {}
        suites = (
            ("app", APP_SOURCE, ("on_dock_action", "on_btn_raw", "on_jpeg_result"),
             APP_STUBS, APP_CASES),
            ("viewer", VIEW_SOURCE,
             ("restore_pending_avatar", "tick", "jpeg_view_request_mode", "jpeg_view_exit",
              "jpeg_view_is_active", "jpeg_view_is_fullscreen"), VIEW_STUBS, VIEW_CASES),
        )
        for suite, path, names, stubs, cases in suites:
            source = path.read_text(encoding="utf-8")
            functions = "\n\n".join(extract_function(source, name) for name in names)
            source_file = cls.workdir / f"{suite}.c"
            source_file.write_text(COMMON + stubs + functions + cases, encoding="utf-8")
            executable = cls.workdir / suite
            result = subprocess.run(
                [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-function", "-Wno-unused-variable",
                 "-I", str(ROOT / "components/core/ports/include"),
                 "-I", str(ROOT / "components/core/services/include"),
                 "-I", str(ROOT / "components/ui/presentation/include"),
                 str(source_file), "-o", str(executable)],
                cwd=cls.workdir, capture_output=True, text=True, timeout=30,
            )
            if result.returncode:
                raise AssertionError(f"{suite} C harness failed to compile:\n{result.stderr}")
            cls.executables[suite] = executable

    def run_case(self, suite: str, name: str) -> None:
        result = subprocess.run(
            [str(self.executables[suite]), name], cwd=self.workdir,
            capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 0, f"{suite}/{name}:\n{result.stdout}{result.stderr}")

    def test_empty_album_prompts_and_owns_navigation_until_dismissed(self) -> None:
        self.run_case("app", "empty")

    def test_saved_album_disables_navigation_before_requesting_fullscreen(self) -> None:
        self.run_case("app", "saved")

    def test_each_function_key_routes_to_album_confirmation(self) -> None:
        self.run_case("app", "press")

    def test_confirm_return_requests_exit_once(self) -> None:
        self.run_case("app", "confirm")

    def test_other_events_and_invalid_buttons_do_not_exit(self) -> None:
        self.run_case("app", "ignored")

    def test_exit_pending_swallows_buttons_and_dock_actions(self) -> None:
        self.run_case("app", "pending")

    def test_home_hold_never_opens_hidden_notices(self) -> None:
        self.run_case("app", "home_hold")

    def test_avatar_update_does_not_trigger_fullscreen_exit(self) -> None:
        self.run_case("app", "avatar")

    def test_successful_upload_keeps_pending_user_exit(self) -> None:
        self.run_case("app", "success_pending")

    def test_failed_upload_does_not_restore_navigation_before_release(self) -> None:
        self.run_case("app", "failure_pending")

    def test_ordinary_upload_failure_defers_navigation_until_avatar_ready(self) -> None:
        self.run_case("app", "ordinary_failure")

    def test_pending_and_decoding_distinguish_fullscreen_from_avatar(self) -> None:
        self.run_case("viewer", "states")

    def test_exit_request_while_fullscreen_queued_is_kept(self) -> None:
        self.run_case("viewer", "queued_exit")

    def test_exit_request_during_decode_is_kept(self) -> None:
        self.run_case("viewer", "decode_exit")

    def test_new_fullscreen_request_clears_previous_exit(self) -> None:
        self.run_case("viewer", "fresh_request")

    def test_replacement_of_queued_fullscreen_keeps_exit_request(self) -> None:
        self.run_case("viewer", "replace_queued")

    def test_replacement_of_active_fullscreen_keeps_exit_request(self) -> None:
        self.run_case("viewer", "replace_active")

    def test_fullscreen_waits_for_user_without_an_extra_exit_result(self) -> None:
        self.run_case("viewer", "resident")


if __name__ == "__main__":
    unittest.main(verbosity=2)
