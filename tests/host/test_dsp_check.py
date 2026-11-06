"""Check silent DSP diagnostic ownership, bounded work and failure cleanup."""
from pathlib import Path
import subprocess, tempfile, unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <setjmp.h>
#include "xiaozhi.h"
typedef void *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef int xz_capture_t;
#define pdPASS 1
#define portMAX_DELAY 0xffffffffu
static bool stopped=true,sem_fail,task_fail,capture_fail,begin_fail,feed_fail,trigger;
static unsigned sem_live,task_live,capture_live,feeds,delays,deletes;
static int64_t clock_us;
static void (*entry)(void*);static void *argument;static jmp_buf suspended;
static bool xiaozhi_stopped_impl(void){return stopped;}
#define xiaozhi_stopped xiaozhi_stopped_impl
static void *xSemaphoreCreateBinary(void){if(sem_fail)return NULL;sem_live++;return(void*)1;}
static void vSemaphoreDelete(void*p){assert(p==(void*)1&&sem_live);--sem_live;}
static int xTaskCreate(void(*fn)(void*),const char*name,unsigned stack,void*arg,unsigned priority,void**out){
 (void)name;assert(stack==6144&&priority==6);if(task_fail)return 0;
 entry=fn;argument=arg;*out=(void*)2;task_live++;return pdPASS;
}
static void vTaskDelete(void*t){assert(t==(void*)2&&task_live);task_live--;deletes++;}
static void vTaskSuspend(void*p){assert(!p);longjmp(suspended,1);}
static void xSemaphoreGive(void*p){assert(p==(void*)1);}
static int xSemaphoreTake(void*p,unsigned ticks){assert(p==(void*)1&&ticks==portMAX_DELAY);if(!setjmp(suspended))entry(argument);return 1;}
static unsigned uxTaskGetStackHighWaterMark(void*p){assert(!p);return 2048;}
static unsigned esp_clk_cpu_freq(void){return 160000000;}
static int64_t esp_timer_get_time(void){clock_us+=250;return clock_us;}
static void vTaskDelay(unsigned ticks){assert(ticks==1);delays++;}
static xz_capture_t *xz_capture_create(void){if(capture_fail)return NULL;capture_live++;return(xz_capture_t*)3;}
static void xz_capture_destroy(xz_capture_t*c){assert(c==(xz_capture_t*)3&&capture_live);capture_live--;}
static bool xz_capture_begin(xz_capture_t*c,unsigned rate){assert(c==(xz_capture_t*)3&&(rate==16000||rate==24000));return !begin_fail;}
static bool xz_capture_feed(xz_capture_t*c,const int16_t*p,size_t n){assert(c==(xz_capture_t*)3&&p&&(n==128||n==192));feeds++;return !feed_fail;}
static bool xz_capture_triggered(xz_capture_t*c){assert(c==(xz_capture_t*)3);return trigger;}
'''
CASES=r'''
int main(void){
 xz_dsp_stats_t s;
 assert(!xiaozhi_dsp_check(NULL));stopped=false;assert(!xiaozhi_dsp_check(&s));stopped=true;
 assert(!sem_live&&!task_live&&!capture_live);
 sem_fail=true;assert(!xiaozhi_dsp_check(&s));sem_fail=false;
 task_fail=true;assert(!xiaozhi_dsp_check(&s));task_fail=false;
 assert(!sem_live&&!task_live&&!capture_live);
 bool *failures[]={&capture_fail,&begin_fail,&feed_fail,&trigger};
 for(unsigned i=0;i<4;i++){*failures[i]=true;assert(!xiaozhi_dsp_check(&s));*failures[i]=false;assert(!sem_live&&!task_live&&!capture_live);}
 feeds=delays=0;assert(xiaozhi_dsp_check(&s));
 assert(s.frames16==64&&s.frames24==64&&s.avg16_us==250&&s.peak24_us==250&&s.cpu_mhz==160&&s.stack_free==2048);
 assert(feeds==128&&delays==128&&!sem_live&&!task_live&&!capture_live&&deletes==5);
 return 0;
}
'''
class DspCheck(unittest.TestCase):
 def test_silent_work_is_bounded_and_resources_return_on_every_failure(self):
  src=(ROOT/'components/platform/xiaozhi/xz_dsp_check.c').read_text()
  src=src[src.index('typedef struct {'):]
  with tempfile.TemporaryDirectory() as d:
   c,e=Path(d)/'test.c',Path(d)/'test';c.write_text(BOUNDARY+src+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(ROOT/'components/platform/xiaozhi/include'),'-I'+str(ROOT/'components/core/ports/include'),str(c),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True,timeout=5)
