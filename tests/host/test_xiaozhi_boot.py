"""Execute the production boot route with hardware boundaries replaced on host."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    start = source.index('static ' + name)
    brace = source.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class XiaoZhiBootTests(unittest.TestCase):
    def test_cold_wake_factory_and_launch_failure(self):
        source = (ROOT / 'components/app/src/app.c').read_text()
        start = source.index('#if PERIPH_DISPLAY && PERIPH_BUTTON && PERIPH_BLE\n    if (factory_mode)')
        end = source.index('#if PERIPH_AUDIO', start)
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#define PASSPORT_XIAOZHI_EDITION 1
#define PERIPH_DISPLAY 1
#define PERIPH_BUTTON 1
#define PERIPH_BLE 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)0)
static atomic_bool s_boot_avatar_ready;
static bool s_button_guide_active, opened, nav, locked, frame_armed, woke_from_deep_sleep;
static unsigned brands;
static void ui_boot_close(void) {}
static void show_startup_brand(void) { brands++; }
static int s_button, attempts, failures, presses, delays, starts, factory_runs, legacy, notes;
static struct { int game_best; } s_profile = {123};
static void finish_wake_frame(void) { assert(locked); frame_armed=true; }
static bool platform_lvgl_lock(int n) { assert(n==500); attempts++; if(failures){failures--;return false;} locked=true;return true; }
static void platform_lvgl_unlock(void) { assert(locked); locked=false; }
static void platform_lvgl_nav_enable(bool enabled) { assert(locked);nav=enabled; }
static bool app_plays_open_xiaozhi(int best) { assert(locked&&s_button_guide_active&&!nav&&best==123);starts++;return opened; }
static bool platform_button_any_pressed(int b) { (void)b;assert(s_button_guide_active&&!locked);return presses-->0; }
static void vTaskDelay(int ms) { assert(!locked);delays+=ms; }
static void note_activity(void) { assert(!locked);notes++; }
static void run_onboard_selftest(void) { factory_runs++; }
'''
        code += function(source, 'void on_boot_voice_opened(') + '\n'
        code += function(source, 'bool start_boot_xiaozhi(') + '\n'
        code += 'static void boot_tail(bool factory_mode) {\n' + source[start:end] + '\nlegacy++;}\n'
        code += r'''
int main(void) {
    opened=true;nav=true;boot_tail(false);
    assert(starts==1&&!nav&&!locked&&!s_button_guide_active&&notes==1&&legacy==0);
    assert(!frame_armed&&!atomic_load(&s_boot_avatar_ready)); // Wait for actual screen.
    locked=true;on_boot_voice_opened(NULL);locked=false;
    assert(frame_armed&&atomic_load(&s_boot_avatar_ready));
    failures=2;presses=3;int old_delays=delays;boot_tail(false);
    assert(starts==2&&notes==2&&!s_button_guide_active&&legacy==0);
    assert(delays-old_delays==2*50+3*20+80); // Held wake key is consumed.
    unsigned before=brands;woke_from_deep_sleep=true;boot_tail(false);assert(brands==before&&starts==3);
    boot_tail(true);assert(factory_runs==1&&starts==3&&legacy==0);
    opened=false;boot_tail(false);
    assert(starts==4&&nav&&!locked&&!s_button_guide_active&&legacy==1);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            c = Path(temp) / 'boot.c'
            c.write_text('#include <stddef.h>\n' + code)
            exe = Path(temp) / 'boot'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(c), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


    def test_brand_frame_audio_order_and_bounded_failure(self):
        source=(ROOT/'components/app/src/app.c').read_text()
        code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define PASSPORT_XIAOZHI_EDITION 1
#define PERIPH_AUDIO 1
#define AUDIO_MIC_PASSTHROUGH_TEST 0
#define UI_BOOT_DURATION_MS 1600
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(...) ((void)samples)
static bool s_button_guide_active,locked,shown=true,deny,draw=true,armed;
static unsigned ms,music,first_music;
static int64_t esp_timer_get_time(void){return (int64_t)ms*1000;}
static bool platform_lvgl_lock(int n){(void)n;if(deny)return false;assert(!locked);locked=true;return true;}
static void platform_lvgl_unlock(void){assert(locked);locked=false;}
static bool ui_boot_open(void){assert(locked&&s_button_guide_active);return shown;}
static bool ui_boot_drawn(void){assert(locked);return draw&&ms>=60;}
static void finish_wake_frame(void){assert(locked);armed=true;}
static void vTaskDelay(unsigned n){assert(!locked);ms+=n;}
static unsigned xiaozhi_play_startup_sound(void){assert(!locked&&s_button_guide_active);first_music=ms;music++;ms+=1120;return 17920;}
"""
        code+=function(source,'void show_startup_brand(')+r"""
int main(void){
 show_startup_brand();assert(armed&&first_music==260&&music==1&&ms==1600&&!locked&&!s_button_guide_active);
 ms=0;draw=false;show_startup_brand();assert(first_music==700&&ms==1820&&music==2&&!locked&&!s_button_guide_active);
 ms=0;shown=false;show_startup_brand();assert(first_music==200&&ms==1320&&music==3&&!locked&&!s_button_guide_active);
 ms=0;deny=true;show_startup_brand();assert(!ms&&music==3&&!s_button_guide_active);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'brand.c').write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all',str(p/'brand.c'),'-o',str(p/'brand')],check=True)
            subprocess.run([str(p/'brand')],check=True)


if __name__ == '__main__':
    unittest.main()
