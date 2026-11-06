"""Run the real sampler and pinned LVGL lock under simulated render contention."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "services/battery.h"
#define PERIPH_DISPLAY 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
typedef uint32_t TickType_t;
static struct {int lvgl_mux;} lvgl_port_ctx={1};
static unsigned render_left_us,boosted_us,render_calls,unlocks;
static int sensor_soc=80,shown_pct=-1;
static int xSemaphoreTakeRecursive(int mutex,TickType_t wait) {
 assert(mutex==1);
 uint64_t budget=wait==portMAX_DELAY?UINT64_MAX:(uint64_t)wait*1000;
 unsigned consumed=render_left_us<budget?render_left_us:(unsigned)budget;
 boosted_us+=consumed;render_left_us-=consumed;
 return render_left_us==0?pdTRUE:0;
}
static void platform_lvgl_unlock(void){unlocks++;}
static battery_filter_t s_batt_f;
static void *s_batt=&s_batt_f;
static int hal_battery_read_soc(void*p){assert(p==s_batt);return sensor_soc;}
static void platform_clicker_set_battery(int p){assert(p==80);}
static int xz_shown_pct;
static void ui_xiaozhi_set_battery(int pct,int level){assert(level==5);xz_shown_pct=pct;}
static void ui_profile_set_battery(int pct,int level){assert(level==5);shown_pct=pct;render_calls++;}
'''
CASES=r'''
int main(void){
 // Demonstrate SDK0's actual infinite timeout on a40ms render; this previously
 // raised LVGL above the voice worker for longer than the24kHz DMA ring.
 render_left_us=40000;assert(lvgl_port_lock(0));assert(boosted_us==40000);
 battery_init(&s_batt_f);boosted_us=0;render_left_us=40000;
 assert(!battery_sample());
 assert(boosted_us<=1000&&shown_pct==-1&&render_calls==0&&unlocks==0);
 assert(battery_percent(&s_batt_f)==80); // Sampling/cache survives UI contention.
 render_left_us=0;sensor_soc=-1;
 assert(battery_sample());assert(shown_pct==80&&xz_shown_pct==80&&render_calls==1&&unlocks==1);
 puts("pinned SDK0 wait reproduced40ms priority inheritance; actual battery sampler bounds it to1ms and retries cached SOC PASS");
}
'''
class BatteryTimerLatencyTest(unittest.TestCase):
 def test_timer_does_not_inherit_full_render_latency(self):
  app=(ROOT/'components/app/src/app.c').read_text()
  port=(ROOT/'managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c').read_text()
  wrapper=(ROOT/'components/platform/platform_esp32/src/lvgl_port_setup.c').read_text()
  code=BOUNDARY+function(port,'lvgl_port_lock')+function(wrapper,'platform_lvgl_lock')+function(app,'battery_sample')+CASES
  with tempfile.TemporaryDirectory() as d:
   c,exe=Path(d)/'battery.c',Path(d)/'battery';c.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(ROOT/'components/core/services/include'),str(c),str(ROOT/'components/core/services/src/battery.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
