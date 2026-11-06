"""Exercise actual serial self-test arbitration and raw key capture."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include "services/factory_test.h"
#include "xiaozhi.h"
#define PERIPH_DISPLAY 1
#define PERIPH_AUDIO 1
#define PERIPH_BLE 0
#define AUDIO_MIC_PASSTHROUGH_TEST 0
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define taskENTER_CRITICAL(p) ((void)p)
#define taskEXIT_CRITICAL(p) ((void)p)
#define MALLOC_CAP_8BIT 4
#define HAL_BTN_PRESS 0
#define HAL_BTN_CLICK 1
#define HAL_BTN_LONG 2
#define UI_ALBUM_PRESS 0
#define UI_ALBUM_CLICK 1
#define UI_ALBUM_LONG 2
#define UI_ALBUM_RETURN_HOME 1
#define UI_ALBUM_DISMISSED 2
#define UI_GAME_KEY_UP 0
#define UI_GAME_KEY_DOWN 1
#define UI_GAME_KEY_EXIT 2
#define UI_GAME_KEY_ENTER 3
typedef int hal_btn_event_t;
typedef int ui_album_action_t;
static atomic_bool s_sleep_preparing,s_deep_sleep_entering,s_factory_command_active;
static atomic_uint s_last_activity_ms;
static bool s_factory_mode, s_btn_test_active, s_button_guide_active,s_button_guide_exit,s_view_nav_restore_pending;
static uint8_t s_btn_test_count[3];
static int s_btn_test_mask,s_power_guard,s_view_exit_ms;
static void *s_audio_owner=(void*)1;
static bool playing,audio_busy,nav=true;
static unsigned commands,takes,gives,forwarded;
static int64_t esp_timer_get_time(void) { return 90000000; }
static unsigned esp_get_free_heap_size(void) { return 17000; }
static unsigned esp_get_minimum_free_heap_size(void) { return 12000; }
static unsigned heap_caps_get_largest_free_block(int caps) { assert(caps==MALLOC_CAP_8BIT); return 11000; }
void xiaozhi_snapshot(xz_view_t *view) {
    *view=(xz_view_t){.phase=XZ_LISTEN,.stage=XZ_STAGE_VOICE,.revision=7,.wifi_saved=true};
    strcpy(view->detail,"PRIVATE_TRANSCRIPT"); strcpy(view->code,"PRIVATE_CODE");
    strcpy(view->hotspot,"PRIVATE_WIFI"); strcpy(view->password,"PRIVATE");
}
bool xiaozhi_stopped(void) { return !playing; }
static void note_activity(void) { s_last_activity_ms=90000; }
static bool app_plays_active(void) { return playing; }
static void app_plays_button(int i,int e) { ++forwarded; }
static void app_plays_cancel_sleep(void) {}
static bool ui_album_active(void) { return false; }
static bool jpeg_view_is_active(void) { return false; }
static bool jpeg_view_is_fullscreen(void) { return false; }
static void jpeg_view_exit(void) {}
static bool capture_ui_blocked(void) { return false; }
static bool platform_lvgl_lock(int n) { return true; }
static void platform_lvgl_unlock(void) {}
static void platform_lvgl_nav_enable(bool v) { nav=v; }
static ui_album_action_t ui_album_button(int i,int e) { return 0; }
static bool ui_game_is_active(void) { return false; }
static void ui_game_key(int k) {}
static void ui_profile_open_licenses(void) {}
static int xSemaphoreTake(void *p,int wait) { takes++; return !audio_busy; }
static void xSemaphoreGive(void *p) { gives++; }
static int run_factory_command(ft_cmd_t cmd,char *o,int n,void *u) {
    assert(s_factory_command_active && (cmd==FT_DSP ? nav : !nav)); ++commands; return 17;
}
'''
CASES=r'''
int main(void) {
    char out[256];
    assert(ft_parse("AT+TEST=VOICE",13)==FT_VOICE);
    assert(ft_parse("  at+test= voice \t",17)==FT_VOICE);
    assert(ft_parse("AT+TEST=VOICEX",14)==FT_NONE);
    assert(ft_parse("AT+TEST=VOICE\0bad",17)==FT_NONE);
    assert(!strcmp(ft_cmd_name(FT_VOICE),"VOICE"));
    playing=true; s_button_guide_active=true; s_sleep_preparing=true; audio_busy=true;
    int status=on_factory_test(FT_VOICE,out,sizeof out,0);
    assert(status==(int)strlen(out) && strstr(out,"phase=6,stage=3,revision=7,wifi_saved=1"));
    assert(strstr(out,"heap=17000,largest=11000,min=12000,uptime_ms=90000,OK\r\n"));
    assert(!strstr(out,"PRIVATE") && !commands && !takes && !gives && nav);
    assert(!s_last_activity_ms && !s_factory_command_active && !s_view_nav_restore_pending);
    char tiny[5]={'x','x','x','x','!'};
    assert(on_factory_test(FT_VOICE,tiny,4,0)==3 && tiny[3]==0 && tiny[4]=='!');
    assert(on_factory_test(FT_VOICE,NULL,0,0)==0);
    assert(ft_parse("AT+TEST=DSP",11)==FT_DSP);
    assert(ft_parse("AT+TEST=DSPX",12)==FT_NONE);
    // DSP is silent, keeps navigation/timer state, but cannot run in a play.
    on_factory_test(FT_DSP,out,sizeof out,0); assert(!commands);
    playing=false; s_sleep_preparing=false;
    assert(on_factory_test(FT_DSP,out,sizeof out,0)==17);
    assert(commands==1 && !takes && !gives && nav && !s_view_nav_restore_pending && !s_last_activity_ms);
    commands=0;
    s_button_guide_active=false; s_sleep_preparing=false; audio_busy=false;
    playing=true; assert(on_factory_test(FT_INFO,out,sizeof out,0)>0 && commands==0);
    playing=false; s_sleep_preparing=true; on_factory_test(FT_AUTO,out,sizeof out,0); assert(!commands&&!s_factory_command_active);
    s_sleep_preparing=false;audio_busy=true;on_factory_test(FT_AUDIO,out,sizeof out,0);assert(!commands&&!s_factory_command_active&&takes==1&&!gives);
    audio_busy=false;assert(on_factory_test(FT_AUTO,out,sizeof out,0)==17);assert(commands==1&&takes==2&&gives==1&&!s_factory_command_active&&s_view_nav_restore_pending);
    s_view_nav_restore_pending=false;
    s_btn_test_active=true; // ordinary boot's AT+TEST=BTN, not factory boot
    for(int i=0;i<3;i++){on_btn_raw(i,HAL_BTN_PRESS,0);on_btn_raw(i,HAL_BTN_CLICK,0);on_btn_raw(i,HAL_BTN_LONG,0);assert(s_btn_test_count[i]==1);}
    assert(s_btn_test_mask==7&&!forwarded);
    s_factory_mode=true;
    for(int i=0;i<3;i++){on_btn_raw(i,HAL_BTN_PRESS,0);on_btn_raw(i,HAL_BTN_CLICK,0);assert(s_btn_test_count[i]==2);}
    s_btn_test_count[0]=255;on_btn_raw(0,HAL_BTN_PRESS,0);assert(s_btn_test_count[0]==255);
    on_btn_raw(-1,HAL_BTN_PRESS,0);on_btn_raw(3,HAL_BTN_PRESS,0);assert(s_btn_test_mask==7);
    return 0;
}
'''
class FactoryCommand(unittest.TestCase):
    def test_arbitration_and_raw_capture(self):
        src=(ROOT/'components/app/src/app.c').read_text()
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'t.c';e=Path(d)/'t'
            c.write_text(BOUNDARY+function(src,'ft_voice_status')+function(src,'on_factory_test')+function(src,'on_btn_raw')+CASES)
            subprocess.run(['cc','-std=c11','-fsanitize=undefined','-fno-sanitize-recover=all','-Werror=implicit-function-declaration',
                            '-I'+str(ROOT/'components/core/services/include'),
                            '-I'+str(ROOT/'components/core/ports/include'),
                            '-I'+str(ROOT/'components/platform/xiaozhi/include'),
                            str(ROOT/'components/core/services/src/factory_test.c'),str(c),'-o',str(e)],check=True)
            subprocess.run([str(e)],check=True)
