from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
DRIVER = ROOT / "components/platform/platform_esp32/src/disp_st7789.c"
LVGL = ROOT / "components/platform/platform_esp32/src/lvgl_port_setup.c"
APP = ROOT / "components/app/src/app.c"


class DisplayBootContractTest(unittest.TestCase):
    def test_backlight_is_claimed_before_panel_reset(self):
        source = DRIVER.read_text(encoding="utf-8")
        create = source.index("hal_display_t *platform_create_display")
        self.assertLess(source.index("bl_init(c);", create), source.index("esp_lcd_panel_reset", create))

    def test_driver_holds_panel_off_until_first_frame(self):
        source = DRIVER.read_text(encoding="utf-8")
        create = source[source.index("hal_display_t *platform_create_display") :]
        self.assertIn("esp_lcd_panel_disp_on_off(c->panel, false)", create)
        self.assertNotIn("esp_lcd_panel_disp_on_off(c->panel, true)", create)

    def test_first_frame_event_enables_panel_without_sync_refresh(self):
        source = LVGL.read_text(encoding="utf-8")
        present = source[source.index("static void first_frame_ready") :]
        self.assertIn("LV_EVENT_REFR_READY", source)
        self.assertNotIn("lv_refr_now", source)
        self.assertLess(present.index("esp_lcd_panel_disp_on_off"), present.index("hal_display_set_backlight"))

    def test_backlight_waits_for_first_frame_gate(self):
        source = APP.read_text(encoding="utf-8")
        display_boot = source[source.index("s_disp = platform_create_display") :]
        self.assertIn("platform_lvgl_arm_first_frame", display_boot)
        self.assertNotIn("hal_display_set_backlight(s_disp, BRIGHT_PCT)", display_boot)


if __name__ == "__main__":
    unittest.main()
