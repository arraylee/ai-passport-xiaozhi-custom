"""Exercise the real battery sampler and icon setter with HAL/LVGL host stubs."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

try:
    from .test_album_interaction import COMMON, ROOT, extract_function
except ImportError:
    from test_album_interaction import COMMON, ROOT, extract_function


STUBS = r"""
#include <stdarg.h>
#include "services/battery.h"
#define PERIPH_DISPLAY 1
#define BATT_BOOT_RETRY_MS 50
#define BATT_BOOT_TIMEOUT_MS 500
#define pdMS_TO_TICKS(ms) (ms)
#define LV_OPA_COVER 255
#define lv_color_hex(value) (value)
#define COL_WHITE 0xF2F5F3u
#define COL_GRAY 0x707D77u
#define COL_BATT_CRIT 0xE0463Au
typedef unsigned lv_color_t;
typedef struct { lv_color_t color; int width; } lv_obj_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
typedef int lv_layer_t, lv_event_t;
typedef struct {int radius,border_width,border_opa,bg_opa;lv_color_t bg_color,border_color;} lv_draw_rect_dsc_t;
typedef struct {lv_obj_t *icon;int level,percent;} ui_battery_icon_t;
static lv_obj_t icon,outline,fill,tip;
static struct {ui_battery_icon_t battery;} P={{&icon,0,-2}};
static int voice_soc=-1,voice_level;
static void ui_xiaozhi_set_battery(int pct,int level){voice_soc=pct;voice_level=level;}
static battery_filter_t s_batt_f;
static void *s_batt = &s_batt_f;
static int mock_soc = -1, soc_reads, mv_reads, renders, locks, unlocks;
static int delay_calls, boot_ready_after_delays = -1, boot_soc;
static int clicker_updates, clicker_pct = -1;
static bool lock_available = true;
static int hal_battery_read_soc(void *battery) {
    CHECK(battery == s_batt); soc_reads++; return mock_soc;
}
int hal_battery_read_mv(void *battery) {
    CHECK(battery == s_batt); mv_reads++; return 4200;
}
static bool platform_lvgl_lock(int timeout) {
    (void)timeout; locks++; return lock_available;
}
static void platform_lvgl_unlock(void) { unlocks++; }
static void platform_clicker_set_battery(int percent) {
    clicker_updates++; clicker_pct = percent;
}
static void vTaskDelay(int ticks) {
    CHECK(ticks == BATT_BOOT_RETRY_MS);
    delay_calls++;
    if (delay_calls == boot_ready_after_delays) mock_soc = boot_soc;
}
static void lv_obj_invalidate(lv_obj_t *obj){CHECK(obj==&icon);renders++;}
static void *lv_event_get_user_data(lv_event_t *e){(void)e;return &P.battery;}
static lv_layer_t *lv_event_get_layer(lv_event_t *e){(void)e;fill.width=0;return NULL;}
static void lv_obj_get_coords(lv_obj_t *obj,lv_area_t*a){CHECK(obj==&icon);*a=(lv_area_t){0,0,23,13};}
static lv_obj_t *lv_obj_get_parent(lv_obj_t *obj){CHECK(obj==&icon);return &outline;}
static lv_color_t lv_obj_get_style_bg_color(lv_obj_t *obj,int part){CHECK(obj==&outline&&part==0);return 0x0B1118;}
static void lv_draw_rect_dsc_init(lv_draw_rect_dsc_t *d){memset(d,0,sizeof *d);}
static void lv_draw_rect(lv_layer_t *l,const lv_draw_rect_dsc_t*d,const lv_area_t *a){
    (void)l;
    if(d->border_width)outline.color=d->border_color;
    else if(a->x1==22)tip.color=d->bg_color;
    else if(a->x1==2){fill.width=a->x2-a->x1+1;fill.color=d->bg_color;}
}

"""


CASES = r"""
static void expect_icon(int level, lv_color_t color, bool hidden) {
    battery_draw(NULL);CHECK(P.battery.level==level);
    CHECK(outline.color==color&&tip.color==color&&(hidden||fill.color==color));
    CHECK((fill.width==0)==hidden);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    battery_init(&s_batt_f);
    ui_profile_set_battery(-1, 0);
    expect_icon(0, COL_GRAY, true);
    renders = 0;
    if (strcmp(argv[1], "initial_invalid") == 0) {
        const int invalid[] = {-1, 101, -1};
        for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
            mock_soc = invalid[i]; battery_sample();
            expect_icon(0, COL_GRAY, true);
        }
        CHECK(soc_reads == 3 && renders == 0 && locks == 0 && unlocks == 0);
        CHECK(s_batt_f.level == 0);
        CHECK(clicker_updates == 0 && clicker_pct == -1);
    } else if (strcmp(argv[1], "zero") == 0) {
        mock_soc = 0; battery_sample();
        expect_icon(1, COL_BATT_CRIT, true);
        CHECK(battery_percent(&s_batt_f) == 0 && renders == 1);
        CHECK(clicker_pct == 0);
    } else if (strcmp(argv[1], "unknown") == 0) {
        ui_profile_set_battery(80, 5);
        ui_profile_set_battery(-1, 0);
        expect_icon(0, COL_GRAY, true);
    } else if (strcmp(argv[1], "eighty") == 0) {
        mock_soc = 0; battery_sample();
        mock_soc = 80; battery_sample();
        expect_icon(5, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 80 + 99) / 100);
        CHECK(battery_percent(&s_batt_f) == 80 && renders == 2);
        CHECK(clicker_pct == 80);
    } else if (strcmp(argv[1], "read_failure") == 0) {
        mock_soc = 80; battery_sample();
        mock_soc = -1; battery_sample();
        mock_soc = 101; battery_sample();
        expect_icon(5, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 80 + 99) / 100);
        CHECK(battery_percent(&s_batt_f) == 80 && renders == 1);
        CHECK(clicker_pct == 80);
    } else if (strcmp(argv[1], "lock_retry") == 0) {
        lock_available = false; mock_soc = 80; battery_sample();
        expect_icon(0, COL_GRAY, true);
        CHECK(renders == 0 && locks == 1 && unlocks == 0);
        CHECK(battery_percent(&s_batt_f) == 80);
        CHECK(clicker_pct == 80);
        lock_available = true; mock_soc = -1; battery_sample();
        expect_icon(5, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 80 + 99) / 100);
        CHECK(renders == 1 && locks == 2 && unlocks == 1); CHECK(voice_soc==80 && voice_level==5);
        CHECK(clicker_pct == 80);
    } else if (strcmp(argv[1], "same_level") == 0) {
        mock_soc = 81; battery_sample();
        expect_icon(5, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 81 + 99) / 100);
        CHECK(clicker_pct == 81);
        mock_soc = 92; battery_sample();
        expect_icon(5, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 92 + 99) / 100);
        CHECK(clicker_pct == 92 && battery_percent(&s_batt_f) == 92);
        mock_soc = 93; battery_sample();
        expect_icon(5, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 93 + 99) / 100);
        CHECK(clicker_pct == 93 && battery_percent(&s_batt_f) == 93);
        CHECK(renders == 3);
    } else if (strcmp(argv[1], "boot_prime") == 0) {
        mock_soc = -1;
        boot_soc = 63;
        boot_ready_after_delays = 3;
        battery_prime_on_boot();
        expect_icon(4, COL_WHITE, false);
        CHECK(fill.width == (BATT_FILL_W * 63 + 99) / 100);
        CHECK(soc_reads == 4 && delay_calls == 3);
        CHECK(clicker_updates == 1 && clicker_pct == 63);
    } else {
        CHECK(false);
    }
    // Periodic sampling reads SOC only; VCELL remains reserved for explicit diagnostics.
    CHECK(mv_reads == 0);
    return 0;
}
"""


class BatteryDisplayTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("a host C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="battery-display-")
        cls.addClassCleanup(temporary.cleanup)
        source_path = Path(temporary.name) / "test.c"
        cls.binary = Path(temporary.name) / "test"
        app = (ROOT / "components/app/src/app.c").read_text(encoding="utf-8")
        ui = (ROOT / "components/ui/presentation/src/ui_profile.c").read_text(encoding="utf-8")
        theme = (ROOT / "components/ui/presentation/include/presentation/ui_theme.h").read_text(encoding="utf-8")
        shared = (ROOT / "components/ui/presentation/src/ui_theme.c").read_text()
        defines = re.findall(r"(?m)^#define BATT_\w+\b[^\n]*", shared)
        source_path.write_text("\n".join([
            COMMON, STUBS,
            *re.findall(r"(?m)^#define UI_(?:BG|INK|MUTED)\b[^\n]*", theme), *defines,
            extract_function(shared,"battery_draw"),extract_function(shared,"ui_theme_battery_set"),
            extract_function(ui, "ui_profile_set_battery"),
            extract_function(app, "battery_sample"),
            extract_function(app, "battery_prime_on_boot"), CASES,
        ]), encoding="utf-8")
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "components/core/services/include"),
            str(source_path), str(ROOT / "components/core/services/src/battery.c"),
            "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def run_case(self, case: str) -> None:
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_initial_invalid_soc_never_displays_zero_or_full(self) -> None:
        self.run_case("initial_invalid")

    def test_valid_zero_is_red_level_one(self) -> None:
        self.run_case("zero")

    def test_unknown_is_gray_without_fill(self) -> None:
        self.run_case("unknown")

    def test_zero_to_eighty_immediately_fills_icon(self) -> None:
        self.run_case("eighty")

    def test_read_failure_preserves_last_valid_soc_without_voltage_fallback(self) -> None:
        self.run_case("read_failure")

    def test_lock_failure_retries_cached_soc_even_if_next_read_fails(self) -> None:
        self.run_case("lock_retry")

    def test_same_color_level_refreshes_continuous_icon_and_clicker_soc(self) -> None:
        self.run_case("same_level")

    def test_boot_prime_stops_after_first_valid_soc_reaches_ui(self) -> None:
        self.run_case("boot_prime")


if __name__ == "__main__":
    unittest.main(verbosity=2)
