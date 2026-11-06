"""Keep timing diagnostics off the live duplex deadline, including RX gaps."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#define HAL_AUDIO_DUPLEX_FRAMES 120
#define HAL_AUDIO_ECHO_GAP 1
static atomic_bool cancel;
static struct {void *audio;} config;
typedef int xz_capture_t;
typedef struct {uint32_t frames,clipped_frames,aligned_frames,candidate_frames,echo_veto_frames,last_clipped_frame,trigger_far_hangover;uint64_t microphone_energy_sum,reference_energy_sum,residual_energy_sum;} xz_bargein_metrics_t;
static int64_t now=100;
static bool echo_live,trigger,feed_ok=true;
static unsigned logged,gaps,feeds,observed;
static int read_result;
static int64_t esp_timer_get_time(void){return now;}
static unsigned uxTaskGetStackHighWaterMark(void*p){(void)p;assert(!echo_live);return 4000;}
static void record(const char *fmt,...){(void)fmt;assert(!echo_live);++logged;}
#define ESP_LOGI(tag,...) record(__VA_ARGS__)
static unsigned xz_capture_gap_count(xz_capture_t*p){assert(p);return gaps;}
static bool xz_capture_observe(xz_capture_t*p,xz_bargein_metrics_t*out){assert(p&&!echo_live);observed++;*out=(xz_bargein_metrics_t){.frames=16};return true;}
static bool xz_capture_triggered(xz_capture_t*p){assert(p);return trigger;}
static int hal_audio_read_echo(void*a,int16_t*out,size_t n,size_t*got){(void)a;assert(echo_live&&n==120);now+=5000;*got=n;memset(out,0,n*4);return read_result;}
static bool xz_capture_discontinuity(xz_capture_t*p){assert(p);++gaps;return true;}
static bool xz_capture_feed(xz_capture_t*p,const int16_t*in,size_t n){assert(p&&in&&n==120);++feeds;now+=3000;return feed_ok;}
'''
CASES=r'''
int main(void){
 xz_capture_t capture=0;uint32_t peak=0;echo_live=true;
 assert(capture_reply(&capture,24000,240,&peak));
 assert(audio_timing.reads==2&&audio_timing.read_peak_us==5000&&audio_timing.read_interval_peak_us==8000&&peak==3000);
 assert(logged==0&&observed==0&&feeds==2);
 read_result=HAL_AUDIO_ECHO_GAP;
 assert(capture_reply(&capture,16000,120,&peak)&&gaps==1&&logged==0);
 echo_live=false;audio_timing_report(&capture,peak,13000);
 assert(logged==2&&observed==1&&!audio_timing.pending);
 audio_timing_report(NULL,0,0);assert(logged==2); // Early failed launch has no capture.
 echo_live=true;read_result=-1;
 assert(!capture_reply(&capture,24000,120,&peak)&&audio_timing.pending&&feeds==3&&logged==2);
 echo_live=false;audio_timing_report(&capture,peak,13000);assert(logged==4);
 echo_live=true;read_result=0;feed_ok=false;
 assert(!capture_reply(&capture,24000,120,&peak));
 echo_live=false;audio_timing_report(&capture,peak,13000);assert(logged==6);
 unsigned before=audio_timing.reads;
 assert(!capture_reply(&capture,22050,120,&peak));
 assert(!capture_reply(&capture,24000,119,&peak));
 cancel=true;assert(capture_reply(&capture,24000,120,&peak));
 assert(audio_timing.reads==before);
 uint32_t value=0;now=(int64_t)UINT32_MAX+9000;timing_peak(&value,1);assert(value==UINT32_MAX);
 value=7;timing_peak(&value,now+1);assert(value==7);
 puts("live duplex: no logging/heap scans on normal, discontinuity or failure paths; deferred timing survives cleanup PASS");
}
'''
class RealtimeDiagnosticsTest(unittest.TestCase):
 def test_production_capture_defers_diagnostics_until_audio_stops(self):
  s=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
  start=s.index('typedef struct {',s.index('// Sole voice-worker-owned'))
  code=BOUNDARY+s[start:s.index('static bool capture_reply(',start)]+function(s,'capture_reply')+CASES
  with tempfile.TemporaryDirectory() as d:
   c,exe=Path(d)/'timing.c',Path(d)/'timing';c.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
