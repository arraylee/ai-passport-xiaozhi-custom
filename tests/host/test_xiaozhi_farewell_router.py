"""Exercise the production outer worker: exactly one audible farewell before parking."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_xz_channel_memory import function

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <setjmp.h>
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
enum { XZ_LISTEN, XZ_FAREWELL, XZ_STANDBY, XZ_WAKING, XZ_ERROR, XZ_RADIO };
typedef struct { unsigned quiet_samples; } xz_quiet_t;
static struct {int phase;} working;
static atomic_bool cancel, requested_resume, requested_end, requested_interrupt, worker_finished;
static atomic_uint avatar;
static unsigned scenario, sessions, prompts, parked, closed, awaiting, wakes;
static uint64_t tick;
static bool live, link;
static jmp_buf exit_worker;
static atomic_int radio_step;
static unsigned streams;
static bool retained;
static void codec_arena_metric(unsigned n){assert(n==8&&retained);}
static bool xz_codec_memory_end(void){assert(retained);retained=false;return true;}
typedef struct {const char *name;} xz_station_t;
static int config;
static void xz_radio_unqueue(void){}
static void xz_radio_forget(void){}
static const xz_station_t *xz_radio_select(int step){
 static xz_station_t a={"A"},b={"B"}; return step?&b:&a;
}
static bool xz_radio_stream(void *cfg,const xz_station_t *station){
 assert(cfg==&config&&!live&&link&&working.phase==XZ_RADIO&&retained);
 if(!streams++) {assert(!strcmp(station->name,"A"));radio_step=1;}
 else {assert(!strcmp(station->name,"B"));requested_end=true;}
 return false;
}
static uint64_t ms(void){return tick;}
static const char *xz_theme_last_error(void){return "主题加载失败\n图片建议128×128以内";}
static void status(int p,const char *s){
 working.phase=p;
 if(p==XZ_FAREWELL){assert(!live&&link&&!strcmp(s,"拜拜"));}
 if(p==XZ_STANDBY){assert(link&&!live);parked++;if(scenario==4)assert(strstr(s,"声音"));}
 if(p==XZ_STANDBY&&scenario==12)assert(!strcmp(s,xz_theme_last_error()));
 if(p==XZ_WAKING)wakes++;
}
static size_t play_farewell_prompt(void){assert(!live&&link&&working.phase==XZ_FAREWELL);prompts++;if(scenario==3)cancel=true;return scenario==4?0:11628;}
static void xz_wifi_idle_power(bool idle){assert(idle&&link&&!live);}
static void xz_wifi_close(void){assert(link);link=false;closed++;}
static void xz_theme_boot(void){}
static void persist_avatar(unsigned n){(void)n;}
static void finish_status(void){assert(!link&&!live);}
static void vTaskSuspend(void *p){(void)p;longjmp(exit_worker,1);}
static void vTaskDelay(unsigned ticks){assert(working.phase==XZ_STANDBY&&link&&!live);tick+=ticks;if(++awaiting==3)requested_resume=true;}
'''
CASES = r'''
static session_end_t run_session(bool *wifi_up,xz_quiet_t *clock,unsigned *missing_replies){
 (void)missing_replies;
 sessions++;if(!*wifi_up){*wifi_up=true;link=true;}assert(link);live=true;working.phase=XZ_LISTEN;
 if(parked){assert(clock->quiet_samples==0);cancel=true;live=false;return SESSION_CLOSED;}
 live=false; // Real run_session has already released its microphone, network session and codecs at return.
 if(scenario==5&&sessions==1){clock->quiet_samples=320000;tick+=5100;return SESSION_RENEW;}
 if(scenario==5)assert(clock->quiet_samples==320000);
 if(scenario==7){tick+=500;return SESSION_RENEW;}
 if(scenario==8){tick+=5000;return SESSION_AUDIO_LOST;}
 if(scenario==12)return SESSION_THEME_FAILED;
 if(scenario>=9){retained=true;if(scenario==10)cancel=true;if(scenario==11)requested_end=true;return SESSION_RADIO;}
 if(scenario==2)return SESSION_PARK;
 if(scenario==6)requested_end=true;
 if(scenario==0||scenario==5){clock->quiet_samples=480000;return SESSION_QUIET_BYE;}
 return SESSION_INTENT_BYE;
}
'''

class FarewellRouter(unittest.TestCase):
    def test_production_worker_once_before_online_standby(self):
        source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        enum=re.search(r'typedef enum \{ SESSION_CLOSED[^;]*;',source).group()
        main=r'''
int main(void){for(scenario=0;scenario<13;scenario++){
 sessions=prompts=parked=closed=awaiting=wakes=0;cancel=requested_resume=requested_end=worker_finished=false;link=live=false;tick=0;
 if(!setjmp(exit_worker))run(NULL);
 assert(worker_finished&&closed==1&&!live&&!link&&!retained);
 assert(prompts==((scenario==2||scenario==6||scenario==7||scenario==8||scenario>=9)?0:1));
 if(scenario==9)assert(streams==2&&sessions==2);
 assert(parked==((scenario==3||scenario==10)?0:1));if(scenario==5)assert(sessions==3);if(scenario==7)assert(sessions==4);if(scenario==8)assert(sessions==3&&wakes==2);
}puts("Quiet, intent, manual end, full stop, output failure, renewal, duplicate stop and rapid-close recovery PASS");}
'''
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'worker.c';binary=Path(tmp)/'worker'
            c.write_text(BOUNDARY+enum+'\n'+CASES+function(source,'run')+'\n'+main)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=4)

if __name__=='__main__':unittest.main()
