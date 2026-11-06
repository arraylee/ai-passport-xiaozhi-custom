"""Execute actual app idle callback; replace hardware shutdown boundaries only."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function
ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "services/profile_ctl.h"
#include "xiaozhi.h"
#define PERIPH_DISPLAY 1
#define PERIPH_BLE 1
#define PERIPH_BUTTON 1
#define PERIPH_AUDIO 1
#define PERIPH_BATTERY 1
#define DEEP_SLEEP_FORCE_DISABLED 0
#define DEEP_SLEEP_TEST_SECONDS 0
#define BRIGHT_PCT 100
#define ESP_OK 0
#define ESP_GPIO_WAKEUP_GPIO_LOW 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static void enter_guard(void);
#define taskENTER_CRITICAL(x) enter_guard()
#define taskEXIT_CRITICAL(x) ((void)(x))
typedef int esp_err_t;
static atomic_bool s_sleep_preparing, s_deep_sleep_entering, s_factory_command_active;
static atomic_uint s_last_activity_ms;
static bool s_factory_mode, s_view_nav_restore_pending, s_button_guide_active;
static unsigned s_view_exit_ms, now_ms, sleeps, cancels, shutdowns, dims;
static int s_power_guard, s_button_wakeup_gpio=0;
static void *s_button, *s_disp=(void*)1, *s_cfg, *s_batt=(void*)1;
static profile_data_t s_profile;
static bool clicked, pending, new_input;
enum { UI_PLAY_XIAOZHI = 1 };
static atomic_int active = UI_PLAY_XIAOZHI;
static xz_view_t voice_view;
void xiaozhi_snapshot(xz_view_t *out) { *out = voice_view; }
static void note_activity(void) { s_last_activity_ms=now_ms; }
static void profile_ui_flush_pending(void) {}
static void log_heap(const char *s) {}
static bool jpeg_view_is_active(void) { return false; }
static bool jpeg_view_decode_busy(void) { return false; }
static bool ui_album_active(void) { return false; }
static bool app_plays_active(void) { return clicked; }
static bool app_plays_clicker_active(void) { return clicked; }
static bool platform_button_any_pressed(void *p) { return false; }
static bool capture_ui_blocked(void) { return false; }
static bool platform_lvgl_lock(unsigned ms) { return true; }
static void platform_lvgl_unlock(void) {}
static bool ui_profile_is_active(void) { return true; }
static void platform_lvgl_nav_enable(bool b) {}
static int64_t esp_timer_get_time(void) { return (int64_t)now_ms*1000; }
static void hal_display_set_backlight(void *p, unsigned b) { ++dims; }
static bool sleep_async_work_finished(void) { return !pending; }
static int esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int mode) { return 0; }
static void app_plays_cancel_sleep(void) { ++cancels; }
static bool platform_button_prepare_deep_sleep(void *p) {
    if (new_input) { note_activity(); atomic_store(&s_sleep_preparing, false); }
    return true;
}
static bool app_plays_commit_sleep(void) { return true; }
static void platform_button_cancel_deep_sleep(void *p) {}
static int hal_battery_shutdown(void *p) { assert(shutdowns++==0); return 0; }
static int audio_stop_for_deep_sleep(void) { assert(shutdowns++==1); return 0; }
static int hal_config_shutdown(void *p) { assert(shutdowns++==2); return 0; }
static int hal_display_shutdown(void *p) { assert(shutdowns++==3); return 0; }
static void esp_deep_sleep_start(void) { ++sleeps; }
'''
CASES=r'''
static bool phone_request_before_lock;
static void enter_guard(void) {
    if (phone_request_before_lock) {
        phone_request_before_lock=false;
        assert(note_portal_activity());
    }
}
static void restore_saved_timeout(int minutes) {
    profile_data_t saved;
    profile_ctl_init(&saved);
    saved.sleep_min=minutes;
    uint8_t blob[PROFILE_BLOB_SIZE];
    assert(profile_serialize(&saved,blob,sizeof blob)==PROFILE_BLOB_SIZE);
    profile_ctl_init(&s_profile);
    assert(profile_deserialize(&s_profile,blob,sizeof blob)==0);
}
int main(int argc, char **argv) {
    int scenario=atoi(argv[1]); clicked=true; restore_saved_timeout(1);
    if(scenario==0) { restore_saved_timeout(0); now_ms=3600000; idle_check_cb(0); assert(!sleeps&&!dims); }
    if(scenario==1) { now_ms=59999; idle_check_cb(0); assert(!sleeps); now_ms=60000; idle_check_cb(0); assert(sleeps==1&&shutdowns==4); }
    if(scenario==2) { restore_saved_timeout(5); now_ms=299999; idle_check_cb(0); assert(!sleeps); now_ms=300000; idle_check_cb(0); assert(sleeps==1); }
    if(scenario==3) { s_factory_mode=true; clicked=false; now_ms=3600000; idle_check_cb(0); assert(!sleeps&&!shutdowns); }
    if(scenario==4) { now_ms=60000; new_input=true; idle_check_cb(0); assert(!sleeps&&!shutdowns&&cancels==1); }
    if(scenario==5) { now_ms=60000; pending=true; idle_check_cb(0); assert(!sleeps&&!shutdowns); pending=false; idle_check_cb(0); assert(sleeps==1); }
    if(scenario==6) { s_factory_command_active=true; now_ms=3600000; idle_check_cb(0); assert(!sleeps&&!shutdowns); }
    if(scenario==7) { profile_ctl_init(&s_profile); now_ms=299999; idle_check_cb(0); assert(!sleeps&&!shutdowns); now_ms=300000; idle_check_cb(0); assert(sleeps==1); }
    if(scenario==8 || scenario==9) {
        const xz_phase_t busy_phases[]={XZ_BIND,XZ_WIFI,XZ_CLOUD,XZ_LISTEN,XZ_THINK,XZ_SPEAK,XZ_WAKING,XZ_FAREWELL};
        voice_view.phase=busy_phases[atoi(argv[2])];
        now_ms=1800000; idle_check_cb(0); assert(!sleeps&&!shutdowns);
        now_ms=1800250; idle_check_cb(0); assert(!sleeps&&!shutdowns);
        voice_view.phase=scenario==8 ? XZ_READY : XZ_ERROR;
        now_ms=1800251; idle_check_cb(0); assert(!sleeps&&!shutdowns);
        now_ms=1860249; idle_check_cb(0); assert(!sleeps&&!shutdowns);
        now_ms=1860250; idle_check_cb(0); assert(sleeps==1);
    }
    if(scenario==10) { atomic_store(&active, -1); voice_view.phase=XZ_SETUP; now_ms=60000; idle_check_cb(0); assert(sleeps==1); }
    if(scenario==11) {
        profile_ctl_init(&s_profile);voice_view.phase=XZ_SETUP;
        now_ms=150000;idle_check_cb(0);assert(!sleeps);
        now_ms=299999;idle_check_cb(0);assert(!sleeps);
        now_ms=300000;idle_check_cb(0);assert(sleeps==1);
    }
    if(scenario==12) {
        voice_view.phase=XZ_SETUP;now_ms=59999;assert(note_portal_activity());
        now_ms=60000;idle_check_cb(0);assert(!sleeps);
        now_ms=119998;idle_check_cb(0);assert(!sleeps);
        now_ms=119999;idle_check_cb(0);assert(sleeps==1&&!note_portal_activity());
    }
    if(scenario==13) {
        restore_saved_timeout(0);voice_view.phase=XZ_SETUP;now_ms=86400000;
        idle_check_cb(0);assert(!sleeps&&!dims&&note_portal_activity());
    }
    if(scenario==14) {
        voice_view.phase=XZ_SETUP;now_ms=60000;phone_request_before_lock=true;
        idle_check_cb(0);assert(!sleeps&&!s_sleep_preparing);
        now_ms=119999;idle_check_cb(0);assert(!sleeps);
        now_ms=120000;idle_check_cb(0);assert(sleeps==1);
    }
    if(scenario==15) {
        profile_ctl_init(&s_profile); voice_view.phase=XZ_LISTEN;
        now_ms=123456;idle_check_cb(0);assert(!sleeps);
        voice_view.phase=XZ_STANDBY;
        now_ms=123706;idle_check_cb(0);assert(!sleeps);
        now_ms=423455;idle_check_cb(0);assert(!sleeps);
        now_ms=423456;idle_check_cb(0);assert(sleeps==1&&shutdowns==4);
    }
    return 0;
}
'''
class DemoIdlePolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src=(ROOT/'components/app/src/app.c').read_text()
        plays=(ROOT/'components/app/src/app_plays.c').read_text()
        cls.temp=tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        c=Path(cls.temp.name)/'test.c';cls.exe=Path(cls.temp.name)/'test'
        c.write_text(BOUNDARY+function(plays,'app_plays_voice_busy')+function(src,'note_portal_activity')+function(src,'idle_check_cb')+CASES)
        subprocess.run(['cc','-std=c11','-fsanitize=undefined','-fno-sanitize-recover=all',
                        '-Werror=implicit-function-declaration',
                        '-I'+str(ROOT/'components/core/services/include'),
                        '-I'+str(ROOT/'components/core/ports/include'),
                        '-I'+str(ROOT/'components/platform/xiaozhi/include'),
                        str(c),str(ROOT/'components/core/services/src/profile_ctl.c'),'-o',str(cls.exe)],check=True)

    def run_scenario(self, scenario, *arguments):
        result=subprocess.run([str(self.exe),str(scenario),*map(str,arguments)],capture_output=True,text=True)
        self.assertEqual(0,result.returncode,result.stdout+result.stderr)

    def test_saved_timeout_and_factory_isolation(self):
        for scenario in range(7):
            with self.subTest(scenario=scenario):
                self.run_scenario(scenario)

    def test_new_profile_stays_awake_for_five_minutes(self):
        self.run_scenario(7)

    def test_busy_phases_protect_sleep_then_ready_and_error_get_fresh_window(self):
        for transition,scenario in [('ready',8),('error',9)]:
            for phase in range(8):
                with self.subTest(transition=transition,phase=phase):
                    self.run_scenario(scenario,phase)

    def test_inactive_voice_page_does_not_hold_device_awake(self):
        self.run_scenario(10)

    def test_waiting_for_phone_uses_device_sleep_and_accepts_activity(self):
        for scenario in (11,12,13,14):
            with self.subTest(scenario=scenario): self.run_scenario(scenario)

    def test_online_standby_sleeps_five_minutes_after_conversation(self):
        self.run_scenario(15)
