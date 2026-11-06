"""Durable source-boundary contracts for app screenshot race cleanup."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[3]
APP_SOURCE = ROOT / "components/app/src/app.c"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function {name} is missing")
    brace = source.find("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"function {name} has no closing brace")


class ScreenCaptureAppContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = APP_SOURCE.read_text(encoding="utf-8")

    def test_disconnect_aborts_jpeg_upload_before_capture_cleanup(self) -> None:
        body = function_body(self.source, "on_ble_disconnected")
        reset = body.find("jpeg_rx_init(&s_jpeg_rx);")
        clear = body.find("atomic_store(&s_jpeg_upload_active, false);")
        cancel = body.find("capture_request_cancel(CAP_CANCEL_DISCONNECT);")
        self.assertGreaterEqual(reset, 0)
        self.assertGreater(clear, reset)
        self.assertGreater(cancel, clear)

    def test_end_boundary_maps_accepted_user_cancel_to_done(self) -> None:
        helper = function_body(self.source, "capture_end_interrupted")
        self.assertIn("reason == CAP_CANCEL_USER ? CFG_ST_DONE", helper)
        self.assertIn("reason == CAP_CANCEL_USER);", helper)
        run_start = function_body(self.source, "capture_run_start")
        self.assertGreaterEqual(run_start.count("capture_end_interrupted(session)"), 2)

    def test_nimble_ui_paths_skip_lvgl_while_capture_owns_it(self) -> None:
        gate = function_body(self.source, "capture_ui_blocked")
        self.assertIn("s_capture_start_pending", gate)
        self.assertIn("s_capture_active", gate)
        self.assertIn("platform_screen_capture_active()", gate)

        config = function_body(self.source, "on_cfg_message")
        self.assertIn("atomic_fetch_or(&s_profile_ui_pending, ui_pending)", config)
        self.assertIn("profile_ui_flush_pending()", config)
        flush = function_body(self.source, "profile_ui_flush_pending")
        self.assertLess(flush.index("if (capture_ui_blocked()) return;"), flush.index("platform_lvgl_lock(10)"))

        token = function_body(self.source, "on_token_bcast")
        # The token balance path remains guarded; retired pet commands have no UI work.
        self.assertEqual(
            token.count("!capture_ui_blocked() && platform_lvgl_lock(0)"), 1
        )
        self.assertNotIn("ui_profile_set_pet", token)

        scan = function_body(self.source, "on_ble_match")
        # 心跳广播已改为静默忽略；扫描回调不再直接操作 LVGL。
        self.assertNotIn("platform_lvgl_lock(0)", scan)
        self.assertNotIn("ui_mario_", scan)

    def test_raw_buttons_are_discarded_before_any_side_effect_during_capture(self) -> None:
        body = function_body(self.source, "on_btn_raw")
        gate = body.find("if (capture_ui_blocked()) return;")
        # Activity and sleep cancellation now share the final-shutdown guard.
        activity = body.find("s_last_activity_ms =")
        factory = body.find("if (s_factory_mode || s_btn_test_active")
        self.assertGreaterEqual(gate, 0)
        self.assertGreater(activity, gate)
        self.assertGreater(factory, gate)
        self.assertGreater(body.find("atomic_exchange(&s_sleep_preparing, false)"), gate)

    def test_capture_keeps_ble_modem_awake_until_transfer_is_terminal(self) -> None:
        run_start = function_body(self.source, "capture_run_start")
        begin = run_start.find("screen_capture_start(&s_capture, capture_id)")
        hold = run_start.find("capture_transport_hold(true)")
        first_packet = run_start.find("capture_send_current_packet()")
        release = run_start.rfind("capture_transport_hold(false)")
        done = run_start.rfind("notify_status(CFG_MSG_SCREEN_CAPTURE, CFG_ST_DONE)")
        self.assertGreater(hold, begin)
        self.assertGreater(first_packet, hold)
        self.assertGreater(release, first_packet)
        self.assertGreater(done, release)

        abort = function_body(self.source, "capture_abort_active")
        self.assertIn("capture_transport_hold(false)", abort)


if __name__ == "__main__":
    unittest.main(verbosity=2)
