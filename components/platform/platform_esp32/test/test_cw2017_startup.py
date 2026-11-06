"""Run the real CW2017 driver against deterministic I2C and monotonic-time fakes."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[4]
PLATFORM = ROOT / "components/platform/platform_esp32"
FIXTURES = Path(__file__).resolve().parent / "cw2017_startup"


class CW2017StartupTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("A host C compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="cw2017-startup-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        for name in ("platform/platform_factory.h", "driver/i2c_master.h",
                     "driver/i2c_types.h", "freertos/FreeRTOS.h",
                     "freertos/task.h", "esp_log.h", "esp_timer.h"):
            header = directory / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "cw2017_stubs.h"\n', encoding="utf-8")
        (directory / "platform/board_config.h").write_text(
            '#pragma once\n#include "' + str(PLATFORM / "include/platform/board_config.h") +
            '"\n#undef CW2017_WRITE_PROFILE\n#define CW2017_WRITE_PROFILE TEST_WRITE_PROFILE\n',
            encoding="utf-8")
        cls.executables = {}
        for write_profile in (0, 1):
            executable = directory / f"cw2017-{write_profile}"
            compiled = subprocess.run([
                compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DTEST_WRITE_PROFILE={write_profile}",
                "-I", str(directory), "-I", str(FIXTURES),
                "-I", str(PLATFORM / "src"),
                "-I", str(ROOT / "components/core/ports/include"),
                str(FIXTURES / "test_startup.c"), "-o", str(executable),
            ], capture_output=True, text=True)
            if compiled.returncode:
                raise RuntimeError(compiled.stdout + compiled.stderr)
            cls.executables[write_profile] = executable

    def run_case(self, name, write_profile, argument=0):
        result = subprocess.run([str(self.executables[write_profile]), name, str(argument)],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_startup_sampling_and_shutdown_with_both_profile_modes(self):
        for write_profile in (0, 1):
            for name in ("normal", "config_read_error", "wake_write_error", "wake_verify_error",
                         "immediate", "readiness",
                         "shutdown", "shutdown_retry", "shutdown_failure", "read_lock"):
                with self.subTest(write_profile=write_profile, case=name):
                    self.run_case(name, write_profile)
            for config in (0xF0, 0x30, 0x01):
                with self.subTest(write_profile=write_profile, config=config):
                    self.run_case("wake", write_profile, config)

    def test_profile_read_errors_never_mutate_the_chip(self):
        for register in (0x0B, 0x10, 0x5F):
            for config in (0x00, 0xF0):
                with self.subTest(register=register, config=config):
                    self.run_case("profile_read_error", 1, register | (config << 8))
        # A mismatch must not hide a later I2C failure and accidentally cause a reset.
        self.run_case("profile_read_error", 1, 0x5F | (1 << 16))

    def test_profile_update_checks_every_written_byte(self):
        for initial_state in range(4):
            with self.subTest(initial_state=initial_state):
                self.run_case("profile_update", 1, initial_state)
        for index in range(80):
            with self.subTest(corrupted_byte=index):
                self.run_case("profile_verify_error", 1, index)
        for name in ("profile_write_error", "profile_verify_read_error", "flag_write_error"):
            with self.subTest(case=name):
                self.run_case(name, 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)
