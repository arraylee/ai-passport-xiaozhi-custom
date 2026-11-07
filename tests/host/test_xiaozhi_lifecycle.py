"""Reproduce the device's 23 KB block limit and radio/task ownership order."""
from pathlib import Path
import subprocess
import os
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define MALLOC_CAP_8BIT 1
#define pdPASS 1
typedef void *TaskHandle_t;
static atomic_bool running, cancel, requested_talk, requested_interrupt, requested_end, requested_resume, welcome_visible, setup_confirmed, worker_finished;
static bool setup_requested, radio_owned;
static TaskHandle_t voice_task;
static struct { unsigned revision; int phase, stage; bool restart_required; } working;
enum { XZ_ERROR, XZ_WIFI, XZ_OFF, XZ_BOOT };
enum { XZ_STAGE_DEVICE };
static bool radio_active=true, pause_fail, resume_fail, task_fail;
static unsigned created, deleted, paused, resumed, task_bytes;
static unsigned aes_initialized, aes_freed, aes_irq_allocations;
typedef struct { unsigned char key[40]; } mbedtls_aes_context;
static bool aes_irq_ready;
static void mbedtls_aes_init(mbedtls_aes_context *ctx) {
 assert(radio_active&&"AES IRQ must be allocated while BLE still owns its interrupt");
 aes_initialized++;memset(ctx,0,sizeof *ctx);
 if(!aes_irq_ready){aes_irq_allocations++;aes_irq_ready=true;}
}
static void mbedtls_aes_free(mbedtls_aes_context *ctx) {
 aes_freed++;memset(ctx,0,sizeof *ctx);
}
static void status(int phase,const char *text) { (void)text; working.phase=phase; working.revision++; }
static unsigned esp_get_free_heap_size(void) { return radio_active ? 31304 : 109000-task_bytes; }
static unsigned heap_caps_get_largest_free_block(int cap) { (void)cap; return radio_active ? 23552 : 100000-task_bytes; }
static void run(void *arg) { (void)arg; }
static bool pause_radio(void) { assert(aes_irq_ready&&aes_initialized==aes_freed&&aes_initialized>paused);paused++; if(pause_fail)return false; radio_active=false; return true; }
static bool resume_radio(void) {
    resumed++; assert(task_bytes==0); // BLE must not compete with the voice stack.
    if(resume_fail)return false; radio_active=true; return true;
}
static struct { bool (*pause_radio)(void); bool (*resume_radio)(void); } config={pause_radio,resume_radio};
static int xTaskCreate(void (*fn)(void *),const char *name,unsigned bytes,void *arg,unsigned prio,TaskHandle_t *out) {
    (void)name;(void)arg;(void)prio;assert(fn==run);
    if(task_fail || bytes>heap_caps_get_largest_free_block(1))return 0;
    assert(!radio_active&&aes_irq_ready&&aes_initialized==aes_freed);created++;task_bytes=bytes;if(out)*out=(void *)1;return pdPASS;
}
static void vTaskDelete(TaskHandle_t task) {
    assert(task==(void *)1 && worker_finished && !radio_active);
    deleted++;task_bytes=0;
}
'''
CASES = r'''
int main(void) {
    assert(xiaozhi_start(false)); // Previously fails at the real startup block limit.
    assert(running && !radio_active && created==1 && paused==1);
    assert(!xiaozhi_start(false) && created==1);
    xiaozhi_service(); assert(!deleted && !resumed && running);
    atomic_store(&worker_finished,true); xiaozhi_service();
    assert(!running && !voice_task && radio_active && deleted==1 && resumed==1);
    xiaozhi_service(); assert(resumed==1);
    for(unsigned i=0;i<20;i++) {
        assert(xiaozhi_start(i%2)); assert(setup_requested==(bool)(i%2));
        cancel=true; worker_finished=true; xiaozhi_service();
        assert(!running && radio_active && !task_bytes && working.phase==XZ_OFF);
    }
    task_fail=true; assert(!xiaozhi_start(false));
    assert(!running && radio_active && !task_bytes && working.phase==XZ_ERROR);
    task_fail=false; pause_fail=true; assert(!xiaozhi_start(false));
    assert(!running && working.restart_required); pause_fail=false;
    assert(xiaozhi_start(false)); resume_fail=true; worker_finished=true;
    xiaozhi_service(); assert(!running && working.restart_required && !task_bytes);
    assert(aes_initialized==24&&aes_freed==24&&aes_irq_allocations==1);
    puts("voice lifecycle: AES IRQ before BLE release, allocation, repeat, cancellation and radio failures passed");
}
'''


class VoiceTaskLifecycleTest(unittest.TestCase):
    def test_device_heap_and_cleanup_order(self):
        source = (ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        marker = 'static void restore_radio(void)'
        tail = source[source.index(marker if marker in source else 'bool xiaozhi_start(bool setup)'):]
        # Old firmware has no servicing hook; the entry assertion must fail first.
        shim = '' if 'void xiaozhi_service(void)' in tail else 'static void xiaozhi_service(void) {}\n'
        with tempfile.TemporaryDirectory() as folder:
            c, exe = Path(folder)/'lifecycle.c', Path(folder)/'lifecycle'
            from test_xz_channel_memory import function
            helper = function(source,'prepare_aes_interrupt') if 'static void prepare_aes_interrupt(' in source else ''
            c.write_text(BOUNDARY+helper+tail+shim+CASES)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function',
                            '-Wno-unused-variable','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

    def test_pinned_sdk_aes_init_allocates_one_persistent_irq_without_encrypting(self):
        from test_xz_channel_memory import function
        sdk=Path(os.environ.get('IDF_PATH',str(Path.home() / 'esp/esp-idf-v5.5.3')))
        common=(sdk/'components/mbedtls/port/aes/esp_aes_common.c').read_text()
        dma=(sdk/'components/mbedtls/port/aes/dma/esp_aes_dma_core.c').read_text()
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define SOC_AES_SUPPORT_DMA 1
#define CONFIG_MBEDTLS_AES_USE_INTERRUPT 1
#define CONFIG_MBEDTLS_AES_INTERRUPT_LEVEL 0
#define ESP_TEE_BUILD 0
#define ESP_OK 0
#define ESP_LOGE(...) ((void)0)
#define ETS_AES_INTR_SOURCE 48
#ifdef bzero
#undef bzero
#endif
#define bzero(p,n) memset(p,0,n)
typedef struct {uint8_t key_bytes;volatile uint8_t key_in_hardware;uint8_t key[32];} esp_aes_context;
typedef struct {unsigned words[21];} StaticSemaphore_t;
typedef int esp_err_t;
static void *op_complete_sem;
static unsigned irq_allocations,semaphore_creations;
static bool fail_irq;
static void esp_aes_complete_isr(void *arg){(void)arg;}
static int esp_intr_level_to_flags(int level){assert(level==0);return 0;}
static esp_err_t esp_intr_alloc(int source,int flags,void (*handler)(void *),void *arg,void *handle){
 assert(source==48&&flags==0&&handler==esp_aes_complete_isr&&!arg&&!handle);
 irq_allocations++;return fail_irq?-1:0;
}
static void *xSemaphoreCreateBinaryStatic(StaticSemaphore_t *buffer){semaphore_creations++;return buffer;}
'''+function(dma,'esp_aes_intr_alloc')+function(common,'esp_aes_init')+function(common,'esp_aes_free')+r'''
int main(int argc,char **argv){
 (void)argv;fail_irq=argc>1;esp_aes_context ctx;memset(&ctx,0x5a,sizeof ctx);
 esp_aes_init(&ctx);assert(irq_allocations==1&&semaphore_creations==1&&op_complete_sem);
 memset(&ctx,0x5a,sizeof ctx);esp_aes_free(&ctx);
 for(unsigned i=0;i<sizeof ctx;i++)assert(((unsigned char *)&ctx)[i]==0);
 esp_aes_init(&ctx);esp_aes_free(&ctx);
 assert(irq_allocations==1&&semaphore_creations==1);
 puts("pinned AES public init: one retained IRQ, no cipher operation, local context cleared PASS");
}
'''
        with tempfile.TemporaryDirectory() as folder:
            c,exe=Path(folder)/'sdk-aes.c',Path(folder)/'sdk-aes'
            c.write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)
            result=subprocess.run([str(exe),'fail'],capture_output=True,timeout=5)
            self.assertEqual(result.returncode,-6,'SDK void init must preserve its fatal IRQ-allocation failure contract')


# Exercise the actual run() startup and completion status path without TLS/HW.
STATUS_BOUNDARY = r'''
#include "xiaozhi.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGI(...) ((void)0)
#define taskENTER_CRITICAL(g) ((void)(g))
#define taskEXIT_CRITICAL(g) ((void)(g))
static int guard;
static xz_view_t working, view;
static atomic_bool cancel;
static atomic_uint avatar;
static bool setup_requested;
static unsigned scenario;
static void allocation_failure_metric(void) {}
static void codec_arena_metric(unsigned step){(void)step;}
typedef struct {} cJSON;
static void xz_text_copy(char *out,size_t n,const char *s) { snprintf(out,n,"%s",s); }
static bool xz_wifi_open(bool setup,atomic_bool *c,void (*status)(xz_phase_t,const char *),char *hotspot,char *password) {
    (void)setup;(void)hotspot;(void)password;
    if(scenario==0)return false;
    if(scenario==1){status(XZ_ERROR,"Wi-Fi 信息保存失败，原网络保留");return false;}
    if(scenario==5){atomic_store(c,true);return false;}
    return true;
}
static cJSON *xz_cloud_cache_load(void){return NULL;}
static cJSON *xz_cloud_refresh_version(cJSON *cached,const atomic_bool *c){(void)c;return cached;}
static bool xz_cloud_time_ready(atomic_bool *c,void (*fn)(xz_phase_t,const char *)){(void)fn;return !atomic_load(c);}
static cJSON *xz_cloud_config(atomic_bool *c,void (*status)(xz_phase_t,const char *),char *code) {
    (void)c;
    if(scenario==2){status(XZ_ERROR,"小智安全证书校验失败");return NULL;}
    if(scenario==3){strcpy(code,"123456");status(XZ_BIND,"等待绑定");status(XZ_ERROR,"绑定超时，请重新获取验证码");return NULL;}
    if(scenario==6)return NULL;
    return (cJSON *)1;
}
'''
STATUS_CASES = r'''
int main(void) {
    for(scenario=0;scenario<7;scenario++) {
        memset(&working,0,sizeof working);cancel=false;
        status(XZ_WIFI,"正在准备连接");run_startup();
        if(scenario==0) assert(strstr(working.detail,"Wi-Fi") && !strstr(working.detail,"连接已结束") && working.stage==XZ_STAGE_WIFI && !working.wifi_saved);
        if(scenario==1) assert(!strcmp(working.detail,"Wi-Fi 信息保存失败，原网络保留"));
        if(scenario==2) assert(!strcmp(working.detail,"小智安全证书校验失败") && working.stage==XZ_STAGE_CLOUD && working.wifi_saved);
        if(scenario==3) assert(!strcmp(working.detail,"绑定超时，请重新获取验证码") && working.stage==XZ_STAGE_BIND && working.wifi_saved);
        if(scenario==4) assert(working.phase!=XZ_ERROR);
        if(scenario==5) assert(working.phase==XZ_OFF && !working.detail[0]);
        if(scenario==6) assert(strstr(working.detail,"小智") && !strstr(working.detail,"重新配网") && working.stage==XZ_STAGE_CLOUD && working.wifi_saved);
    }
    puts("startup errors preserve stage/detail, saved Wi-Fi and cancellation PASS");
}
'''

class StartupStatusTest(unittest.TestCase):
    def test_wifi_cloud_errors_and_cancel_keep_their_own_reason(self):
        from test_xz_channel_memory import function
        source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        run=function(source,"run_session")
        startup=run[run.index("  if (!warm) {"):run.index("  // MQTT storage")]
        if "static void finish_status(" in source:
            finish=function(source,"finish_status")
        else:
            tail=run[run.index("  if (working.restart_required)"):run.index('  ESP_LOGI(TAG, "voice cleanup')]
            finish="static void finish_status(void) {\n"+tail+"}\n"
        code=(STATUS_BOUNDARY+function(source,"status")+function(source,"reconnect_status")+finish+
              "static void xz_wifi_idle_power(bool idle){(void)idle;}\nstatic void run_startup(void) { cJSON *cloud=NULL;bool warm=false, online=false,*wifi_up=&online;\n"+startup+
              "(void)cloud;(void)cloud_cached;return;finish:finish_status();}\n"+STATUS_CASES)
        with tempfile.TemporaryDirectory() as folder:
            c,exe=Path(folder)/"startup.c",Path(folder)/"startup"
            c.write_text(code)
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-fsanitize=undefined",
                "-I",str(ROOT/"components/platform/xiaozhi/include"),
                "-I",str(ROOT/"components/core/ports/include"),str(c),"-o",str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)


if __name__ == '__main__':
    unittest.main()
