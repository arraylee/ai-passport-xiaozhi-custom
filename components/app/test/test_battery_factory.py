"""Run the real factory battery check against a virtual startup clock."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

try:
    from .test_album_interaction import COMMON, ROOT, extract_function
except ImportError:
    from test_album_interaction import COMMON, ROOT, extract_function


STUBS = r"""
#include "services/factory_test.h"
#define pdMS_TO_TICKS(ms) (ms)
typedef uint32_t TickType_t;
static int battery;
static void *s_batt = &battery;
static int soc_reads, mv_reads, delays, waited_ms, ready_ms;
static int ready_soc = 80, pending_soc = -1;
int64_t esp_timer_get_time(void) { return 123456000 + (int64_t)waited_ms * 1000; }
void vTaskDelay(TickType_t ticks) {
    CHECK(ticks == 100);
    waited_ms += (int)ticks;
    delays++;
    CHECK(waited_ms <= 4000);
}
int hal_battery_read_soc(void *device) {
    CHECK(device != NULL && device == s_batt);
    soc_reads++;
    return waited_ms >= ready_ms ? ready_soc : pending_soc;
}
int hal_battery_read_mv(void *device) {
    CHECK(device != NULL && device == s_batt);
    mv_reads++;
    return 3900;
}
"""


CASES = r"""
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const char *expected = "+TEST:BATT,present=1,soc=80,mv=3900,PASS\r\n";
    bool expected_pass = true;
    int expected_wait = 0;
    int expected_reads = 1;
    if (strcmp(argv[1], "missing") == 0 || strcmp(argv[1], "disabled") == 0) {
        if (strcmp(argv[1], "missing") == 0) s_batt = NULL;
        expected = "+TEST:BATT,present=0,soc=-1,mv=-1,FAIL\r\n";
        expected_pass = false; expected_reads = 0;
    } else if (strcmp(argv[1], "ready") == 0) {
        /* First read is already valid: do not delay an AT response. */
    } else if (strcmp(argv[1], "startup") == 0) {
        ready_ms = expected_wait = 3000; expected_reads = 31;
    } else if (strcmp(argv[1], "timeout") == 0) {
        ready_ms = 5000; expected_wait = 4000; expected_reads = 41;
        expected = "+TEST:BATT,present=1,soc=-1,mv=3900,FAIL\r\n";
        expected_pass = false;
    } else if (strcmp(argv[1], "zero") == 0) {
        ready_soc = 0;
        expected = "+TEST:BATT,present=1,soc=0,mv=3900,FAIL\r\n";
        expected_pass = false;
    } else if (strcmp(argv[1], "out_of_range") == 0) {
        pending_soc = 101; ready_ms = expected_wait = 100; expected_reads = 2;
    } else {
        CHECK(false);
    }
    char output[128]; int written = 0;
    CHECK(ft_line_batt(output, sizeof output, &written) == expected_pass);
    CHECK(strcmp(output, expected) == 0);
    CHECK(written == (int)strlen(expected));
    CHECK(waited_ms == expected_wait && delays == expected_wait / 100);
    CHECK(soc_reads == expected_reads);
    CHECK(mv_reads == (expected_reads > 0 ? 1 : 0));
    return 0;
}
"""


class BatteryFactoryTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("a host C compiler is required")
        temporary = tempfile.TemporaryDirectory(prefix="battery-factory-")
        cls.addClassCleanup(temporary.cleanup)
        source_path = Path(temporary.name) / "test.c"
        app = (ROOT / "components/app/src/app.c").read_text(encoding="utf-8")
        source_path.write_text("\n".join([
            COMMON, STUBS, extract_function(app, "ft_line_batt"), CASES,
        ]), encoding="utf-8")
        cls.binaries = {}
        for enabled in (0, 1):
            binary = Path(temporary.name) / f"test-{enabled}"
            cls.binaries[enabled] = binary
            subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DPERIPH_BATTERY={enabled}",
                "-I", str(ROOT / "components/core/services/include"),
                str(source_path), str(ROOT / "components/core/services/src/factory_test.c"),
                "-o", str(binary),
            ], check=True, capture_output=True, text=True)

    def run_case(self, case: str, enabled: int = 1) -> None:
        result = subprocess.run([str(self.binaries[enabled]), case],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_chip_never_reads_or_waits(self) -> None:
        self.run_case("missing")

    def test_valid_reading_returns_immediately(self) -> None:
        self.run_case("ready")

    def test_startup_ready_after_three_seconds_passes(self) -> None:
        self.run_case("startup")

    def test_invalid_soc_times_out_after_four_seconds(self) -> None:
        self.run_case("timeout")

    def test_valid_zero_fails_factory_threshold_without_waiting(self) -> None:
        self.run_case("zero")

    def test_out_of_range_soc_is_retried(self) -> None:
        self.run_case("out_of_range")

    def test_disabled_battery_never_reads_or_waits(self) -> None:
        self.run_case("disabled", enabled=0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
