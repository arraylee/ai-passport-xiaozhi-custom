"""Execute demo's actual station sequence with controlled hardware/manual results."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define PERIPH_BATTERY 1
#define PERIPH_AUDIO 1
#define UI_ST_PASS 1
#define UI_ST_FAIL 2
#define UI_ST_RUN 3
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(n) (n)
#define TONE_PASS_OK "pass"
#define TONE_FAIL_ALARM "alarm"
static int s_st_fails, scenario, rows, colors, alarms, audio_runs;
static void *s_id, *s_batt;
static bool result_pass;
static void hal_identity_get_sn(void *p, char *out, size_t n) { snprintf(out,n,"test-device"); }
static bool platform_lvgl_lock(int ms) { return true; }
static void platform_lvgl_unlock(void) {}
static void ui_selftest_open(const char *s) { assert(!strcmp(s,"test-device")); }
static void ui_selftest_set_item(const char *s,int status) { if(status!=UI_ST_RUN)rows++; }
static void ui_selftest_set_item_text(const char *s,int status,const char *f) { assert(!strcmp(f,"SKPH")); rows++; }
static void ui_selftest_battery(int b) { assert(b==80); }
static int hal_battery_read_soc(void *p) { return 80; }
static void ui_selftest_prompt(const char *p) {}
static void ui_selftest_color(uint32_t c) { const uint32_t rgb[]={0xff0000,0xff00,0xff}; assert(c==rgb[colors++]); }
static void ui_selftest_pattern(bool on) { assert(!on); }
static void ui_selftest_result(bool pass) { result_pass=pass; }
static void ft_run_audio(int *ret,int *mag,int *peak) { *ret=0; *mag=100; *peak=100; audio_runs++; }
static bool line(char *o,int n,int *w) { *w=snprintf(o,n,"+TEST:FAKE,PASS\r\n"); return true; }
static bool ft_line_info(char *o,int n,int *w) { return line(o,n,w); }
static bool ft_line_batt(char *o,int n,int *w) { return line(o,n,w); }
static bool ft_line_ble(char *o,int n,int *w) { line(o,n,w); return scenario!=1; }
static bool ft_line_id(char *o,int n,int *w) { return line(o,n,w); }
static int ft_batt_present(void) { return 1; }
static int ft_fmt_i2c(char *o,int n,int a,int b,bool *p) { assert(a&&b); *p=true; return snprintf(o,n,"I2C\n"); }
static int ft_fmt_audio(char *o,int n,int a,int b,int c,bool *p) { *p=true; return snprintf(o,n,"AUDIO\n"); }
static void identity_fields_present(bool *a,bool *b,bool *c,bool *d) { *a=*b=*c=*d=true; }
static int st_wait_confirm(int ms) { assert(ms==30000); return scenario==2?0:scenario==3?-1:2; }
static bool st_capture_twice(int ms) { assert(ms==20000); return scenario!=4; }
static void play_tone(const char *s) { assert(!strcmp(s,result_pass?TONE_PASS_OK:TONE_FAIL_ALARM)); alarms++; }
'''
CASES=r'''
int main(int argc,char **argv) {
    scenario=atoi(argv[1]);
    run_onboard_selftest();
    assert(rows==8 && audio_runs==1 && alarms==1);
    assert(result_pass==(scenario==0));
    assert(s_st_fails==(scenario?1:0));
    assert(colors==((scenario==2 || scenario==3)?1:3));
    return 0;
}
'''
class OnboardSelftest(unittest.TestCase):
    def test_station_sequence(self):
        src=(ROOT/'components/app/src/app.c').read_text()
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'test.c';e=Path(d)/'test'
            c.write_text(BOUNDARY+function(src,'st_emit_row')+function(src,'run_onboard_selftest')+CASES)
            subprocess.run(['cc','-std=c11','-fsanitize=undefined','-Wall','-Werror',str(c),'-o',str(e)],check=True)
            for i in range(5):
                with self.subTest(scenario=i):
                    r=subprocess.run([str(e),str(i)],check=True,capture_output=True,text=True,timeout=5)
                    self.assertIn(f'+TEST:RESULT,{"FAIL" if i else "PASS"},fails={1 if i else 0}',r.stdout)
