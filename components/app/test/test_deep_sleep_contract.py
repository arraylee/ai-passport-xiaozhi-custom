"""Source contracts for idle deep sleep and ADC function-key wakeup."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
APP = (ROOT / "components/app/src/app.c").read_text(encoding="utf-8")
BOARD = (ROOT / "components/platform/platform_esp32/include/platform/board_config.h").read_text(encoding="utf-8")
BUTTON = (ROOT / "components/platform/platform_esp32/src/btn_iot_button.c").read_text(encoding="utf-8")
DISPLAY = (ROOT / "components/platform/platform_esp32/src/disp_st7789.c").read_text(encoding="utf-8")
AUDIO = (ROOT / "components/platform/platform_esp32/src/audio_es8311.c").read_text(encoding="utf-8")
BATTERY = (ROOT / "components/platform/platform_esp32/src/battery_cw2017.c").read_text(encoding="utf-8")
BLE = (ROOT / "components/platform/platform_esp32/src/ble_config.c").read_text(encoding="utf-8")


class DeepSleepContractTest(unittest.TestCase):
    def test_idle_enters_gpio_deep_sleep(self) -> None:
        self.assertIn("#define DEEP_SLEEP_TEST_SECONDS  0", BOARD)
        self.assertIn("DEEP_SLEEP_TEST_SECONDS * 1000u", APP)
        self.assertIn("esp_deep_sleep_enable_gpio_wakeup(", APP)
        self.assertIn("ESP_GPIO_WAKEUP_GPIO_LOW", APP)
        self.assertIn("esp_deep_sleep_start()", APP)
        self.assertNotIn("s_screen_off", APP)

    def test_disabling_sleep_does_not_skip_fullscreen_navigation_restore(self) -> None:
        callback = APP[APP.index("static void idle_check_cb"):]
        restore = callback.index("s_view_nav_restore_pending")
        disabled = callback.index("#if DEEP_SLEEP_FORCE_DISABLED")
        self.assertLess(restore, disabled)
        self.assertIn("platform_lvgl_nav_enable(true);", callback[restore:disabled])

    def test_all_adc_function_keys_share_wakeup_node(self) -> None:
        self.assertIn(".btn_wakeup_gpio = 0", BOARD)
        self.assertIn("platform_button_any_pressed", BUTTON)

    def test_adc_polling_stops_and_gpio_is_stable_before_sleep(self) -> None:
        self.assertIn("iot_button_stop()", BUTTON)
        self.assertIn(".mode = GPIO_MODE_INPUT", BUTTON)
        self.assertIn(".pull_up_en = GPIO_PULLUP_ENABLE", BUTTON)
        self.assertGreaterEqual(BUTTON.count("gpio_get_level(c->wake_gpio)"), 2)
        prepare = APP.index("platform_button_prepare_deep_sleep(s_button)")
        sleep = APP.index("esp_deep_sleep_start()", prepare)
        self.assertLess(prepare, sleep)

    def test_navigation_attaches_before_ui_and_enables_after_wakeup_release(self) -> None:
        attach = APP.index("platform_lvgl_attach_buttons(btn, lvdisp);")
        ui = APP.index("ui_profile_create();")
        self.assertLess(attach, ui)
        self.assertIn("if (woke_from_deep_sleep || PASSPORT_XIAOZHI_EDITION) platform_lvgl_nav_enable(false);", APP)
        release = APP.index("while (platform_button_any_pressed(btn))")
        enable = APP.index("platform_lvgl_nav_enable(true);", release)
        self.assertLess(release, enable)

    def test_sleep_quiesces_async_work_before_peripherals(self) -> None:
        prepare = APP.index("atomic_store(&s_sleep_preparing, true)")
        async_done = APP.index("sleep_async_work_finished()", prepare)
        battery = APP.index("hal_battery_shutdown(s_batt)", async_done)
        audio = APP.index("audio_stop_for_deep_sleep()", battery)
        ble = APP.index("hal_config_shutdown(s_cfg)", battery)
        display = APP.index("hal_display_shutdown(s_disp)", ble)
        sleep = APP.index("esp_deep_sleep_start()", display)
        self.assertEqual([prepare, async_done, battery, audio, ble, display, sleep],
                         sorted([prepare, async_done, battery, audio, ble, display, sleep]))
        self.assertIn("s_jpeg_upload_active", APP)
        self.assertIn("capture_request_cancel(CAP_CANCEL_DISCONNECT)", APP)
        self.assertIn("jpeg_view_decode_busy()", APP)

    def test_music_task_acknowledges_stop_before_codec_shutdown(self) -> None:
        self.assertIn("s_music_idle_sig", APP)
        self.assertIn("atomic_load(&s_music_playing)", APP)
        self.assertIn("xSemaphoreTake(s_music_idle_sig, pdMS_TO_TICKS(300))", APP)
        self.assertIn("esp_codec_dev_close(c->dev)", AUDIO)
        self.assertIn("ES8311_SUSPEND_SEQ", AUDIO)
        self.assertIn("ES8311_SUSPEND_CHECK", AUDIO)
        self.assertIn("es_force_suspend_once(c)", AUDIO)
        self.assertIn("attempt <= 2", AUDIO)
        self.assertIn("es_release_i2s_pins(c)", AUDIO)
        self.assertIn("es_release_i2c_pins(c)", AUDIO)

    def test_display_enters_sleep_and_holds_backlight_low(self) -> None:
        self.assertIn("esp_lcd_panel_disp_on_off(c->panel, false)", DISPLAY)
        self.assertIn("esp_lcd_panel_io_tx_param(c->io, 0x10", DISPLAY)
        self.assertIn("gpio_set_level(c->bl_gpio, 0)", DISPLAY)
        self.assertIn("gpio_deep_sleep_hold_en()", DISPLAY)
        self.assertIn("gpio_deep_sleep_hold_dis()", DISPLAY)
        self.assertIn("st_hold_spi_sleep_levels(c)", DISPLAY)
        self.assertIn("st_release_sleep_holds(c)", DISPLAY)

    def test_battery_and_ble_are_quiesced(self) -> None:
        self.assertIn("CW_CONFIG_SLEEP", BATTERY)
        self.assertIn("config == CW_CONFIG_SLEEP", BATTERY)
        self.assertIn("attempt <= 2", BATTERY)
        self.assertIn("CW2017 已从休眠状态唤醒", BATTERY)
        self.assertIn("ble_gap_disc_cancel()", BLE)
        self.assertIn("ble_gap_adv_stop()", BLE)
        self.assertIn("ble_gap_terminate", BLE)
        self.assertIn("if (!s_shutting_down) queue_adv_restart();", BLE)
        for function_name in ("queue_adv_restart", "start_adv"):
            with self.subTest(function=function_name):
                body = BLE.split(f"static void {function_name}(void) {{", 1)[1].split("\n}", 1)[0]
                guard = re.search(r"if\s*\((.*?)\)\s*return\s*;", body, re.DOTALL)
                self.assertIsNotNone(guard, "Advertising needs an early-return guard")
                # Shutdown must independently block each entry point, regardless
                # of the order of other sync/connection/queue conditions.
                self.assertIn("s_shutting_down", [term.strip() for term in guard.group(1).split("||")])

    def test_sleep_manager_is_not_tied_to_display_and_ble(self) -> None:
        self.assertIn("#if PERIPH_BUTTON", APP[:APP.index("// 空闲 sleep_min 分进入深度睡眠") + 10])
        self.assertNotIn("#if PERIPH_DISPLAY && PERIPH_BLE\n// 空闲", APP)


if __name__ == "__main__":
    unittest.main(verbosity=2)
