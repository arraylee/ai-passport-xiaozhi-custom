"""Exercise production pause/resume ordering and partial shutdown failures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define BLE_HS_CONN_HANDLE_NONE 65535
#define pdMS_TO_TICKS(x) (x)
static atomic_bool s_voice_host_ready, s_capture_params_ready;
static int sync_ticks=2, sync_timeout, waited;
static void vTaskDelay(int ms) {
    assert(ms==10); waited++;
    if(!sync_timeout && sync_ticks>0 && !--sync_ticks) s_voice_host_ready=true;
}
static bool s_stack_ready, s_voice_paused, s_voice_radio_fault;
static bool s_conn_registration_ready, s_host_synced, s_subscribed, s_capture_subscribed;
static int s_handle, s_conn_registration_event=1, s_adv_restart_event=1, s_adv_retry_callout=1, s_capture_params_event=1;
static int s_reasm, s_conn=1, s_mtu_generation;
static void *s_reasm_timer=(void *)1, *s_disconnect_user;
static void (*s_disconnect_cb)(void *);
static int stopped, stop_error, deinit_error, resume_error, stop_count, deinit_count, resume_count, shutdown_error;
static void capture_params_connection_changed_host(void) {assert(stopped && s_conn==BLE_HS_CONN_HANDLE_NONE);}
static int cfg_shutdown(void *p) { assert(p == &s_handle && s_stack_ready && s_voice_host_ready); return shutdown_error; }
static int nimble_port_stop(void) { stop_count++; if (!stop_error) stopped=1; return stop_error; }
static void esp_timer_stop(void *p) { assert(stopped && p); }
static void esp_timer_delete(void *p) { assert(stopped && p); }
static void ble_clicker_deinit_after_host_stop(void) { assert(stopped); }
static void ble_npl_event_deinit(int *event) { assert(stopped && *event); *event=0; }
static void ble_npl_callout_deinit(int *callout) { assert(stopped && *callout); *callout=0; }
static void frame_reasm_reset(int *p) { assert(p == &s_reasm); }
static int nimble_port_deinit(void) {
    assert(stopped && !s_stack_ready && !s_reasm_timer);
    assert(!s_conn_registration_event && !s_adv_restart_event && !s_adv_retry_callout && !s_capture_params_event && !s_capture_params_ready);
    deinit_count++; return deinit_error;
}
static void platform_create_ble_config(void *a, void *b) {
    assert(!a && !b && s_voice_paused && !s_stack_ready); resume_count++;
    s_voice_host_ready=false;sync_ticks=2;
    if (!resume_error) { s_stack_ready=true; stopped=0;
        s_conn_registration_event=s_adv_restart_event=s_adv_retry_callout=s_capture_params_event=1; s_capture_params_ready=true;
        s_reasm_timer=(void *)1; }
}
'''
CASES = r'''
int main(int argc, char **argv) {
    (void)argv;
    // A boot-disabled radio has nothing to release.
    assert(platform_ble_voice_pause() && !stop_count);
    s_stack_ready=true;
    if(argc == 2) stop_error=1;
    if(argc == 3) deinit_error=1;
    if(argc == 4) resume_error=1;
    if(argc == 6) sync_timeout=1;
    if(argc == 7) shutdown_error=1;
    bool ok=platform_ble_voice_pause();
    if(argc == 7) {assert(!ok && s_voice_radio_fault && !stop_count && !deinit_count && s_stack_ready);return 0;}
    if(argc == 6) {
        assert(!ok&&s_voice_radio_fault&&stop_count==0&&waited==200);
        s_voice_host_ready=true;
        assert(!platform_ble_voice_pause()&&!platform_ble_voice_resume()&&!stop_count);
        return 0;
    }
    if(stop_error || deinit_error) {
        assert(!ok && s_voice_radio_fault);
        assert(!platform_ble_voice_pause() && stop_count==1);
        assert(!platform_ble_voice_resume() && !resume_count);
        return 0;
    }
    assert(ok && !s_stack_ready && s_voice_paused && deinit_count==1);
    assert(platform_ble_voice_pause() && stop_count==1);
    if(argc == 5) sync_timeout=1;
    ok=platform_ble_voice_resume();
    if(resume_error || sync_timeout) {
        assert(!ok && s_voice_radio_fault);
        s_voice_host_ready=true;
        assert(!platform_ble_voice_pause() && !platform_ble_voice_resume());
        assert(resume_count==1); return 0;
    }
    assert(ok && s_stack_ready && !s_voice_paused);
    assert(platform_ble_voice_resume() && resume_count==1);
    assert(platform_ble_voice_pause() && platform_ble_voice_resume());
    assert(deinit_count==2 && resume_count==2);
}
'''


class VoiceRadioLifecycleTest(unittest.TestCase):
    def test_ready_only_after_sync_callback(self):
        code = (ROOT / 'components/platform/platform_esp32/src/ble_config.c').read_text()
        callback = code[code.index('static void on_sync(void)'):code.index('static void on_reset(int reason)')]
        fake = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
static atomic_bool s_voice_host_ready;
static bool s_host_synced;
static unsigned char s_addr_type;
static void ble_hs_id_infer_auto(int x,unsigned char *out){(void)x;*out=1;}
static void ble_clicker_host_synced(void){assert(!s_voice_host_ready&&s_host_synced);}
static void start_adv(void){assert(!s_voice_host_ready);}
'''
        with tempfile.TemporaryDirectory() as folder:
            source,exe=Path(folder)/'ready.c',Path(folder)/'ready'
            source.write_text(fake+callback+'int main(void){on_sync();assert(s_voice_host_ready);}')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(source),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

    def test_repeat_and_fail_closed(self):
        code = (ROOT / 'components/platform/platform_esp32/src/ble_config.c').read_text()
        start = code.index('static bool wait_voice_host_ready(void)') if 'static bool wait_voice_host_ready(void)' in code else code.index('bool platform_ble_voice_pause(void)')
        functions = code[start:code.index('\n#else', start)]
        with tempfile.TemporaryDirectory() as folder:
            source, exe = Path(folder) / 'radio.c', Path(folder) / 'radio'
            source.write_text(BOUNDARY + functions + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(source), '-o', str(exe)], check=True)
            for case in range(7):
                subprocess.run([str(exe)] + ['fault'] * case, check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
