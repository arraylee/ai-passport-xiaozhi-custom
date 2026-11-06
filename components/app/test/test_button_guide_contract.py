"""Source contracts for the one-time cold-boot binding page."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
APP = (ROOT / "components/app/src/app.c").read_text(encoding="utf-8")
GUIDE = (ROOT / "components/ui/presentation/src/ui_button_guide.c").read_text(encoding="utf-8")


class ButtonGuideContractTest(unittest.TestCase):
    def test_legacy_guide_is_disabled_for_xiaozhi_edition(self) -> None:
        self.assertIn("bool show_boot_guide = false;", APP)
        self.assertIn(
            "show_boot_guide = !PASSPORT_XIAOZHI_EDITION && !factory_mode && !woke_from_deep_sleep && !has_user_configured_content();",
            APP,
        )
        self.assertIn('strcmp(s_profile.name, "FoloToy") != 0', APP)
        self.assertIn("s_profile.subtitle[0] != '\\0'", APP)
        self.assertIn('strcmp(s_profile.avatar_name, "default") == 0', APP)
        self.assertIn("avatar_store_first_name(initial_avatar)", APP)
        self.assertIn("strcmp(s_profile.avatar_name, initial_avatar) == 0", APP)
        self.assertIn("jpeg_store_has_valid(JPEG_SLOT_AVATAR)", APP)
        self.assertIn("jpeg_store_has_valid(JPEG_SLOT_FULL)", APP)
        self.assertIn("rtttl_validate(", APP)
        self.assertIn("while (!platform_lvgl_lock(500))", APP)
        self.assertIn("扫码绑定页已请求退出,等待截图/绘制释放 LVGL 后关闭", APP)
        self.assertNotIn("GUIDE_COUNT_DEFAULT", APP)
        self.assertNotIn("guide_remaining", APP)

    def test_entering_home_does_not_persist_a_binding_marker(self) -> None:
        self.assertNotIn("BINDING_DONE_KV_KEY", APP)
        self.assertNotIn("load_binding_done", APP)
        self.assertNotIn("save_binding_done", APP)
        self.assertLess(
            APP.index("s_kv = platform_create_kv(&board);"),
            APP.index("!has_user_configured_content()"),
        )
        self.assertLess(
            APP.index("avatar_store_init()"),
            APP.index("!has_user_configured_content()"),
        )

    def test_deep_sleep_wakeup_skips_guide_and_enters_home(self) -> None:
        self.assertIn("esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO", APP)
        self.assertIn("factory_mode = !woke_from_deep_sleep", APP)
        self.assertIn("while (platform_button_any_pressed(btn))", APP)

    def test_at_commands_no_longer_expose_guide_count(self) -> None:
        self.assertNotIn('at_after(b, "guide_count,")', APP)
        self.assertNotIn("guide_remaining", APP)

    def test_binding_reset_restores_only_user_content_defaults(self) -> None:
        self.assertIn('at_is(a, "binding,reset")', APP)
        self.assertIn("static int at_binding_reset(char *out, int n)", APP)
        self.assertIn("profile_ctl_init(&defaults)", APP)
        self.assertIn("memcpy(s_profile.name, defaults.name", APP)
        self.assertIn("memcpy(s_profile.subtitle, defaults.subtitle", APP)
        self.assertIn("memcpy(s_profile.avatar_name, defaults.avatar_name", APP)
        self.assertIn("hal_kv_erase(s_kv, BOOT_RTTTL_KV_KEY)", APP)
        self.assertIn("jpeg_store_clear(JPEG_SLOT_AVATAR)", APP)
        self.assertIn("jpeg_store_clear(JPEG_SLOT_FULL)", APP)
        self.assertIn('"+OK binding_reset,reboot\\r\\n"', APP)

    def test_guide_is_binding_qrcode(self) -> None:
        self.assertIn("binding_url_build(payload, sizeof payload, sn, key)", GUIDE)
        self.assertIn("lv_qrcode_create", GUIDE)
        self.assertIn("lv_qrcode_set_data(qr, payload)", GUIDE)
        self.assertIn('"扫码开启配置"', GUIDE)
        self.assertIn('"右侧任意按键 · 跳过"', GUIDE)
        self.assertIn("蓝牙连上自动进入主页", GUIDE)
        self.assertIn("hal_config_on_connect(cfg_src, on_ble_connected, NULL)", APP)
        self.assertNotIn("s_countdown", GUIDE)
        self.assertNotIn("秒后进入主页", GUIDE)
        self.assertIn("lv_obj_delete_async(old)", GUIDE)
        self.assertNotIn("shadow_width", GUIDE)
        self.assertIn("lv_obj_set_style_clip_corner(s_scr, false, 0)", GUIDE)
        self.assertIn("lv_obj_set_style_radius(s_scr, 0, 0)", GUIDE)
        self.assertNotIn("add_edge_callout", GUIDE)
        self.assertIn("while (!s_button_guide_exit && !atomic_load(&s_config_connected))", APP)
        self.assertIn("e == HAL_BTN_PRESS && index >= 0 && index < 3", APP)


if __name__ == "__main__":
    unittest.main(verbosity=2)
