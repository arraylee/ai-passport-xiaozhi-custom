"""Build the production temporary codec allocator against deterministic heap/tasks."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
static unsigned heap_allocs,heap_frees;
static bool fragmented,reserve_fail,alignment_gap;
static unsigned reserve_offset;
static size_t reserve_limit=25600,reserve_bytes;
static unsigned small_fail;
static bool require_small_wipe;
static struct {void *pointer;size_t size;} small_heap[64];
static unsigned char *reserve_storage,*reserve_pointer;
static void *current_task=(void *)1;
static void *system_malloc(size_t n){if(fragmented&&n>=24000)return NULL;if(n<=64&&small_fail){small_fail--;return NULL;}heap_allocs++;void *p=malloc(n);if(p&&n<=64){unsigned i;for(i=0;i<64&&small_heap[i].pointer;i++){}assert(i<64);small_heap[i].pointer=p;small_heap[i].size=n;}return p;}
static void *system_calloc(size_t n,size_t s){if(n&&s>SIZE_MAX/n)return NULL;void *p=system_malloc(n*s);if(p)memset(p,0,n*s);return p;}
static void *system_realloc(void *p,size_t n){if(fragmented&&n>=24000)return NULL;return realloc(p,n);}
static void system_free(void *p){if(p){for(unsigned i=0;i<64;i++)if(small_heap[i].pointer==p){if(require_small_wipe)for(size_t b=0;b<small_heap[i].size;b++)assert(((unsigned char *)p)[b]==0);small_heap[i].pointer=NULL;break;}heap_frees++;free(p);}}
static void *heap_caps_aligned_alloc(size_t a,size_t n,unsigned cap){assert(a==16&&cap==1);if(reserve_fail||alignment_gap)return NULL;heap_allocs++;return aligned_alloc(a,n);}
static void *heap_caps_malloc(size_t n,unsigned cap){
 assert(cap==1);if(reserve_fail||n>reserve_limit)return NULL;reserve_bytes=n;assert(!reserve_storage);heap_allocs++;
 reserve_storage=aligned_alloc(16,n+16);assert(reserve_storage);reserve_pointer=reserve_storage+reserve_offset;return reserve_pointer;
}
static void heap_caps_free(void *p){
 if(p==reserve_pointer&&reserve_storage){for(unsigned i=0;i<reserve_bytes;i++)assert(reserve_pointer[i]==0);heap_frees++;free(reserve_storage);reserve_storage=reserve_pointer=NULL;}
 else system_free(p);
}
static void *xTaskGetCurrentTaskHandle(void){return current_task;}
#define malloc system_malloc
#define calloc system_calloc
#define realloc system_realloc
#define free system_free
#define __real_esp_mbedtls_mem_calloc system_calloc
#define __real_esp_mbedtls_mem_free system_free
'''
CASES=r'''
int main(void){
 assert(xz_codec_memory_begin());
 fragmented=true; // TLS has enough total bytes, but no24KiB contiguous block.
 void *wrapper=media_lib_module_calloc("AUD_Codec",1,56);
 void *state=media_lib_module_malloc("AUD_Codec",24524);
 assert(wrapper&&state&&"codec state must reuse the pre-network contiguous arena");
 assert(((uintptr_t)state%16)==0&&((uintptr_t)wrapper%16)==0);
 for(unsigned i=0;i<56;i++)assert(((unsigned char *)wrapper)[i]==0);
 memset(state,0x42,24524);
 xz_codec_memory_stats_t stats;xz_codec_memory_snapshot(&stats);
 assert(stats.capacity==24576&&stats.used>=24524&&stats.peak==stats.used&&stats.failures==0);
 assert(stats.small_used==56&&stats.small_live==1&&stats.small_peak==56);
 assert(!media_lib_module_malloc("AUD_Codec",2048));
 xz_codec_memory_snapshot(&stats);assert(stats.failures==1&&stats.last_failure==2048);
 unsigned before=heap_frees;
 media_lib_free(state);media_lib_free(wrapper);assert(heap_frees==before+1);
 xz_codec_memory_snapshot(&stats);assert(stats.used==0&&stats.small_used==0&&stats.small_live==0);
 // Free-list coalescing must support the opposite order and both codec sizes.
 for(unsigned pass=0;pass<100;pass++){
  void *a=media_lib_module_malloc("AUD_Codec",8),*b=media_lib_module_malloc("AUD_Codec",17784);
  assert(a&&b);media_lib_free(a);media_lib_free(b);
  a=media_lib_module_malloc("AUD_Codec",24524);assert(a);media_lib_free(a);
 }
 void *p=media_lib_module_malloc("AUD_Codec",128);assert(p);memset(p,0x31,128);
 void *grown=media_lib_module_realloc("AUD_Codec",p,1024);assert(grown);
 for(unsigned i=0;i<128;i++)assert(((unsigned char *)grown)[i]==0x31);
 assert(!media_lib_module_realloc("AUD_Codec",grown,26000));
 assert(((unsigned char *)grown)[0]==0x31);media_lib_free(grown);
 assert(!media_lib_module_calloc("AUD_Codec",SIZE_MAX,2));
 // Other tasks/modules must retain system allocator ownership.
 current_task=(void *)2;assert(!media_lib_module_malloc("AUD_Codec",24524));
 void *ordinary=media_lib_module_malloc("AUD_Codec",100);assert(ordinary);
 current_task=(void *)1;before=heap_frees;media_lib_free(ordinary);assert(heap_frees==before+1);
 assert(!media_lib_module_malloc("Other",24524));
 void *other=media_lib_module_calloc("Other",1,100);assert(other);
 other=media_lib_module_realloc("Other",other,200);assert(other);media_lib_free(other);
 p=media_lib_module_malloc("AUD_Codec",128);assert(p);before=heap_frees;
 current_task=(void *)2;
 assert(!media_lib_module_realloc("AUD_Codec",p,256));assert(!xz_codec_memory_end());
 media_lib_free(p);assert(heap_frees==before); // Free routes by pointer, not task/module.
 current_task=(void *)1;
 p=media_lib_module_malloc("AUD_Codec",128);assert(p);before=heap_frees;
 media_lib_free((unsigned char *)p+1);assert(heap_frees==before);
 media_lib_free(p);media_lib_free(p);assert(heap_frees==before); // No underflow/system free.
 assert(!media_lib_module_malloc("AUD_Codec",SIZE_MAX));
 assert(!xz_codec_memory_begin()); // An active arena cannot be replaced/leaked.
 before=heap_frees;assert(xz_codec_memory_end());assert(heap_frees==before+1);
 // Every session returns its region; no permanent BSS heap reservation.
 fragmented=false;
 for(unsigned i=0;i<20;i++){assert(xz_codec_memory_begin());p=media_lib_module_malloc("AUD_Codec",24524);assert(p);media_lib_free(p);assert(xz_codec_memory_end());}
 reserve_fail=true;assert(!xz_codec_memory_begin());reserve_fail=false;
 assert(xz_codec_memory_begin());p=media_lib_module_malloc("AUD_Codec",8);assert(p);
 assert(!xz_codec_memory_end()); // Leaked SDK allocations are reported and reclaimed.
 xz_codec_memory_snapshot(&stats);assert(stats.used==0&&stats.small_used==0&&stats.small_live==0);
 puts("codec arena: fragmented system heap, two codecs, align/calloc/realloc, owner/module fallbacks, repeated close and failed reserve PASS");
}
'''
LEGACY=r'''
typedef struct {size_t capacity,used,peak,failures,last_failure;} xz_codec_memory_stats_t;
static bool xz_codec_memory_begin(void){return true;}
static bool xz_codec_memory_end(void){return true;}
static void xz_codec_memory_snapshot(xz_codec_memory_stats_t *s){memset(s,0,sizeof *s);}
void *media_lib_module_malloc(const char *m,size_t n){(void)m;return malloc(n);}
void *media_lib_module_calloc(const char *m,size_t n,size_t s){(void)m;return calloc(n,s);}
void *media_lib_module_realloc(const char *m,void *p,size_t n){(void)m;return realloc(p,n);}
void media_lib_free(void *p){free(p);}
'''


class CodecArenaTest(unittest.TestCase):
 def test_radio_worker_borrows_decoder_storage_without_tls_ownership(self):
  self._run_case(r'''
int main(void){
 assert(!xz_codec_memory_radio_enter());
 for(unsigned pass=0;pass<100;pass++){
  current_task=(void*)1;assert(xz_codec_memory_begin());
  assert(xz_codec_memory_network_only());assert(xz_codec_memory_loan_begin());
  void *rx=__wrap_esp_mbedtls_mem_calloc(1,17058),*tx=__wrap_esp_mbedtls_mem_calloc(1,1024);
  assert(rx&&tx&&arena_contains(rx)&&arena_contains(tx));
  current_task=(void*)2;assert(xz_codec_memory_radio_enter());
  assert(!xz_codec_memory_radio_enter()&&!xz_codec_memory_loan_begin()&&!xz_codec_memory_loan_end());
  void *foreign_tls=__wrap_esp_mbedtls_mem_calloc(1,2048);
  assert(foreign_tls&&!arena_contains(foreign_tls));__wrap_esp_mbedtls_mem_free(foreign_tls);
  unsigned sizes[]={68,1968,4224,2304,2312,4224,2304,2312,8192};
  void *mp3[9];unsigned shared=0;
  for(unsigned i=0;i<9;i++){mp3[i]=media_lib_module_calloc("AUD_Codec",1,sizes[i]);assert(mp3[i]);if(arena_contains(mp3[i]))shared+=sizes[i];}
  assert(shared>=6000&&mp3[8]==radio_work);
  current_task=(void*)3;assert(!xz_codec_memory_radio_enter());xz_codec_memory_radio_leave();
  assert(radio_owner==(void*)2);
  current_task=(void*)1;__wrap_esp_mbedtls_mem_free(rx);__wrap_esp_mbedtls_mem_free(tx);
  assert(xz_codec_memory_loan_end()); // Decoder can finish its buffered tail after HTTP cleanup.
  current_task=(void*)2;for(unsigned i=0;i<9;i++)media_lib_free(mp3[i]);
  xz_codec_memory_radio_leave();assert(!radio_owner);
  current_task=(void*)1;assert(xz_codec_memory_end());
 }
 assert(heap_allocs==heap_frees);
 puts("100 asynchronous radio workers: shared decoder tail, isolated TLS ownership, joined cleanup PASS");
}
''')
 def test_radio_keeps_tls_reserve_while_mp3_uses_separate_heap(self):
  self._run_case(r'''
int main(void){
 for(unsigned pass=0;pass<100;pass++){
  assert(xz_codec_memory_begin());
  void *voice=media_lib_module_malloc("AUD_Codec",24524);assert(voice);
  assert(!xz_codec_memory_network_only());media_lib_free(voice);
  current_task=(void*)2;assert(!xz_codec_memory_network_only());current_task=(void*)1;
  assert(xz_codec_memory_network_only());
  assert(xz_codec_memory_loan_begin());
  void *rx=__wrap_esp_mbedtls_mem_calloc(1,17058),*tx=__wrap_esp_mbedtls_mem_calloc(1,1024);
  assert(rx&&tx&&arena_contains(rx)&&arena_contains(tx));
  unsigned sizes[]={68,1968,4224,2304,2312,4224,2304,2312,8192};
  void *mp3[9];
  unsigned shared=0;
  for(unsigned i=0;i<9;i++){mp3[i]=media_lib_module_calloc("AUD_Codec",1,sizes[i]);assert(mp3[i]);if(arena_contains(mp3[i]))shared+=sizes[i];}
  assert(shared>=6000);
  assert(mp3[8]==radio_work&&radio_work_live);
  assert(media_lib_module_realloc("AUD_Codec",mp3[8],8192)==mp3[8]);
  assert(!xz_codec_memory_network_only());
  assert(!xz_codec_memory_loan_end());
  __wrap_esp_mbedtls_mem_free(rx);__wrap_esp_mbedtls_mem_free(tx);assert(xz_codec_memory_loan_end());
  assert(!xz_codec_memory_loan_begin()); // Decoder still occupies the shared tail.
  for(unsigned i=0;i<9;i++)media_lib_free(mp3[i]);
  assert(xz_codec_memory_end());
 }
 assert(heap_allocs==heap_frees);
 puts("100 radio handovers keep exact TLS buffers contiguous while MP3 uses independently freed heap PASS");
}
''')
 def test_https_borrows_reserved_storage_without_exposing_it_to_heap_fragmentation(self):
  self._run_case(r'''
int main(void){
 assert(!xz_codec_memory_loan_begin());
 for(unsigned pass=0;pass<100;pass++){
  reserve_offset=(pass%4)*4;reserve_fail=false;assert(xz_codec_memory_begin());
  void *codec=media_lib_module_malloc("AUD_Codec",24524);assert(codec);
  assert(!xz_codec_memory_loan_begin());media_lib_free(codec);
  fragmented=reserve_fail=true; // Re-reserving the arena after HTTPS would fail.
  assert(xz_codec_memory_loan_begin());assert(!xz_codec_memory_loan_begin());
  void *record=__wrap_esp_mbedtls_mem_calloc(1,16717);
  void *handshake=__wrap_esp_mbedtls_mem_calloc(1,4096);
  assert(record&&handshake&&arena_contains(record)&&arena_contains(handshake));
  for(unsigned i=0;i<16717;i++)assert(!((unsigned char *)record)[i]);
  assert(!xz_codec_memory_loan_end());
  // Concurrent MQTT uses ordinary heap, including frees on the HTTP task.
  current_task=(void*)2;void *mqtt=__wrap_esp_mbedtls_mem_calloc(1,2048);
  assert(mqtt&&!arena_contains(mqtt));assert(!xz_codec_memory_loan_begin());
  current_task=(void*)1;__wrap_esp_mbedtls_mem_free(mqtt);
  // A second TLS buffer may spill into ordinary heap without poisoning stats.
  void *spill=__wrap_esp_mbedtls_mem_calloc(1,8192);assert(spill&&!arena_contains(spill));
  __wrap_esp_mbedtls_mem_free(handshake);__wrap_esp_mbedtls_mem_free(record);
  __wrap_esp_mbedtls_mem_free(spill);assert(xz_codec_memory_loan_end());
  assert(!xz_codec_memory_loan_end());
  xz_codec_memory_stats_t s;xz_codec_memory_snapshot(&s);assert(!s.used&&!s.failures);
  // Both real codec state sizes still fit after repeated TLS allocation churn.
  codec=media_lib_module_malloc("AUD_Codec",24524);assert(codec);media_lib_free(codec);
  codec=media_lib_module_malloc("AUD_Codec",17784);assert(codec);media_lib_free(codec);
  assert(xz_codec_memory_end());
 }
 assert(!__wrap_esp_mbedtls_mem_calloc(SIZE_MAX,2));
 assert(heap_allocs==heap_frees);
 puts("100 HTTPS/Opus handovers with unavailable re-reservation, concurrent MQTT and exact TLS record buffers PASS");
}
''')
 def test_retry_24k_region_tracks_small_wrapper_failures_and_reclaims_sdk_leaks(self):
  cases=r'''
int main(void){
 reserve_limit=24576;alignment_gap=true;fragmented=true;require_small_wipe=true;
 for(unsigned pass=0;pass<100;pass++){
  reserve_offset=(pass%4)*4;unsigned a=heap_allocs,f=heap_frees;
  assert(xz_codec_memory_begin()&&"retry has only a24KiB allocation class remaining");
  void *w=media_lib_module_calloc("AUD_Codec",1,56),*p=media_lib_module_malloc("AUD_Codec",24524);
  assert(w&&p&&((uintptr_t)p%16)==0);memset(p,0x73,24524);
  assert(!media_lib_module_malloc("AUD_Codec",24524)); // No system fallback for state.
  media_lib_free(p);media_lib_free(w);
  reserve_pointer[0]=0x55;reserve_pointer[24575]=0x55;
  assert(xz_codec_memory_end()&&!reserve_storage&&heap_allocs==a+2&&heap_frees==f+2);
 }
 assert(xz_codec_memory_begin());
 small_fail=1;unsigned a=heap_allocs;assert(!media_lib_module_calloc("AUD_Codec",1,56));assert(heap_allocs==a);
 void *w=media_lib_module_malloc("AUD_Codec",8);assert(w);memset(w,0x52,8);
 unsigned f=heap_frees;current_task=(void *)2;
 assert(!media_lib_module_realloc("AUD_Codec",w,32));assert(!xz_codec_memory_end());
 current_task=(void *)1;
 media_lib_free((unsigned char *)w+1);assert(((unsigned char *)w)[0]==0x52);
 assert(!media_lib_module_realloc("AUD_Codec",(unsigned char *)w+1,32));
 assert(!media_lib_module_realloc("Other",w,32));
 small_fail=1;assert(!media_lib_module_realloc("AUD_Codec",w,32));assert(((unsigned char *)w)[0]==0x52);
 w=media_lib_module_realloc("AUD_Codec",w,32);assert(w&&((unsigned char *)w)[0]==0x52);
 void *large=media_lib_module_realloc("AUD_Codec",w,24524);assert(large&&((unsigned char *)large)[0]==0x52);
 w=media_lib_module_realloc("AUD_Codec",large,32);assert(w&&((unsigned char *)w)[0]==0x52);
 media_lib_free(w);
 // Fixed tracking capacity must fail closed; the rejected request must not leak.
 void *small[32];unsigned count=0;
 while(count<32&&(small[count]=media_lib_module_malloc("AUD_Codec",8)))count++;
 assert(count==8);a=heap_allocs;
 xz_codec_memory_stats_t stats;xz_codec_memory_snapshot(&stats);
 assert(stats.small_live==8&&stats.small_used==64&&stats.small_peak==64);
 assert(!media_lib_module_malloc("AUD_Codec",8)&&heap_allocs==a);
 for(unsigned i=0;i<count;i++)media_lib_free(small[i]);
 xz_codec_memory_snapshot(&stats);assert(stats.small_live==0&&stats.small_used==0);
 // Foreign task/module ordinary pointers are never captured by session cleanup.
 current_task=(void *)2;void *foreign=media_lib_module_malloc("AUD_Codec",8);assert(foreign);
 current_task=(void *)1;void *other=media_lib_module_malloc("Other",8);assert(other);
 // The real decoder leaks its8-byte wrapper when the state allocation fails.
 w=media_lib_module_calloc("AUD_Codec",1,8);assert(w);
 large=media_lib_module_malloc("AUD_Codec",24524);assert(large);
 assert(!media_lib_module_malloc("AUD_Codec",17784));media_lib_free(large);
 f=heap_frees;assert(!xz_codec_memory_end()&&heap_frees==f+2&&!reserve_storage);
 xz_codec_memory_snapshot(&stats);assert(stats.small_live==0&&stats.small_used==0);
 require_small_wipe=false;f=heap_frees;media_lib_free(foreign);media_lib_free(other);assert(heap_frees==f+2);
 assert(xz_codec_memory_begin());assert(xz_codec_memory_end());
 assert(heap_allocs==heap_frees);
 puts("24KiB retry100cycles, tracked wrapper OOM/realloc/owner/quota and SDK leak reclamation PASS");
}
'''
  self._run_case(cases)
 def test_exact_retry_region_is_usable_without_aligned_allocator_search_gap(self):
  cases=r'''
int main(void){
 // Actual C3 retry: voice28KiB consumes the30288-byte block; a separate
 // exact24KiB ordinary allocation block remains. TLSF memalign needs
 // an additional leading-gap search size, so that legacy request fails.
 alignment_gap=true;
 for(unsigned pass=0;pass<100;pass++){
  reserve_offset=(pass%4)*4;unsigned a=heap_allocs,f=heap_frees;
  assert(xz_codec_memory_begin()&&"exact ordinary24KiB block must be usable after voice stack allocation");
  xz_codec_memory_stats_t stats;xz_codec_memory_snapshot(&stats);
  assert(stats.capacity>=24560&&stats.capacity<=24576&&stats.capacity%16==0);
  void *wrapper=media_lib_module_calloc("AUD_Codec",1,56),*state=media_lib_module_malloc("AUD_Codec",24524);
  assert(wrapper&&state&&((uintptr_t)wrapper%16)==0&&((uintptr_t)state%16)==0);
  memset(state,0x42,24524);media_lib_free(state);media_lib_free(wrapper);
  xz_codec_memory_snapshot(&stats);assert(stats.used==0&&stats.peak==stats.capacity);
  media_lib_free(reserve_pointer);assert(heap_frees==f+1); // Prefix/header is owned, not a system-free pointer.
  size_t prefix=(size_t)(arena-reserve_pointer);memset(reserve_pointer,0x55,prefix);
  reserve_pointer[24575]=0x66; // End must wipe prefix AND unused tail, then free original base.
  assert(xz_codec_memory_end()&&!reserve_storage&&heap_allocs==a+2&&heap_frees==f+2);
 }
 puts("exact retry region, internal16align offsets0/4/8/12, real encoder requests and100 complete ownership returns PASS");
}
'''
  self._run_case(cases)
 def test_codec_allocations_survive_fragmentation_without_stealing_other_owners(self):
  self._run_case(CASES)
 def _run_case(self,cases):
  component=ROOT/'components/platform/xiaozhi';production=component/'xz_codec_memory.c'
  with tempfile.TemporaryDirectory(prefix='passport-codec-arena-') as folder:
   folder=Path(folder);(folder/'freertos').mkdir()
   (folder/'esp_heap_caps.h').write_text('#define MALLOC_CAP_8BIT 1\n')
   (folder/'freertos/FreeRTOS.h').write_text('typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define taskENTER_CRITICAL(g) ((void)(g))\n#define taskEXIT_CRITICAL(g) ((void)(g))\n')
   (folder/'freertos/task.h').write_text('typedef void *TaskHandle_t;\n')
   code=BOUNDARY+(production.read_text() if production.exists() else LEGACY)+cases
   src,exe=folder/'arena.c',folder/'arena';src.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Werror','-fsanitize=undefined',
    '-I',str(folder),'-I',str(component),str(src),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)

class CodecArenaLifecycleTest(unittest.TestCase):
 def test_production_worker_reserves_before_network_and_releases_after_every_failure(self):
  from test_xz_channel_memory import function
  component=ROOT/'components/platform/xiaozhi'
  source=(component/'xiaozhi.c').read_text();run=function(source,'run_session')
  startup=run[run.index('  working.stage = XZ_STAGE_VOICE;'):run.index('  // MQTT storage')]
  close_at=run.index('  if (codec_memory_owned)',run.index('finish:'))
  end=run[close_at:run.index('  persist_avatar(',close_at)]
  stubs=r'''
#include "xiaozhi.h"
#include <stdatomic.h>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static xz_view_t working;
static atomic_bool cancel;
static bool setup_requested;
enum {XZ_OFFICIAL_AVATAR=2,SESSION_THEME_FAILED=9,SESSION_RADIO=10};
static int outcome;
static bool xz_theme_pending_boot(void){return false;}
static bool xz_theme_download(const atomic_bool *c){(void)c;assert(0);return false;}
static const char *xz_theme_last_error(void){return "theme failure";}
static void xz_theme_cancel_download(void){assert(0);}
static void persist_avatar(unsigned a){(void)a;assert(0);}
static void esp_restart(void){assert(0);}
static unsigned scenario,probe_calls,wifi_calls,cloud_calls,close_calls;
typedef struct {} cJSON;
static void codec_arena_metric(unsigned step){(void)step;}
static void status(xz_phase_t phase,const char *detail){(void)detail;working.phase=phase;}
static bool codec_probe(void){
 probe_calls++;void *a=media_lib_module_calloc("AUD_Codec",1,56),*b=media_lib_module_malloc("AUD_Codec",24524);
 assert(a&&b);media_lib_free(b);media_lib_free(a);return scenario!=2;
}
static bool xz_wifi_open(bool setup,atomic_bool *c,void (*fn)(xz_phase_t,const char *),char *hotspot,char *password){
 (void)setup;(void)fn;(void)hotspot;(void)password;wifi_calls++;
 assert(arena&&probe_calls==1);fragmented=true;
 if(scenario==4)atomic_store(c,true);
 return scenario!=3&&scenario!=4;
}
static cJSON *xz_cloud_cache_load(void){return NULL;}
static cJSON *xz_cloud_refresh_version(cJSON *cached,const atomic_bool *c){(void)c;return cached;}
static bool xz_cloud_time_ready(atomic_bool *c,void (*fn)(xz_phase_t,const char *)){(void)fn;return !atomic_load(c);}
static cJSON *xz_cloud_config(atomic_bool *c,void (*fn)(xz_phase_t,const char *),char *code){
 (void)fn;(void)code;cloud_calls++;assert(arena&&fragmented);
 void *a=media_lib_module_malloc("AUD_Codec",24524);assert(a);media_lib_free(a);
 if(scenario==5)atomic_store(c,true);return NULL;
}
static void xz_wifi_idle_power(bool idle){(void)idle;}
'''
  cases=r'''
int main(void){for(scenario=0;scenario<6;scenario++){
 fragmented=false;reserve_fail=scenario==1;cancel=false;probe_calls=wifi_calls=cloud_calls=close_calls=0;
 unsigned before=heap_frees;run_startup();assert(!arena);
 assert(heap_frees==before+(scenario==1?0U:2U));
 assert(probe_calls==(scenario==1?0U:1U));
 assert(wifi_calls==(scenario==1||scenario==2?0U:1U));
 assert(cloud_calls==(scenario==0||scenario==5?1U:0U));
 }puts("production lifecycle: reserve/probe before WiFi, TLS-compatible owner reuse, per-conversation arena release on cancellation/failure PASS");}
'''
  with tempfile.TemporaryDirectory(prefix='passport-codec-arena-lifecycle-') as folder:
   folder=Path(folder);(folder/'freertos').mkdir()
   (folder/'esp_heap_caps.h').write_text('#define MALLOC_CAP_8BIT 1\n')
   (folder/'freertos/FreeRTOS.h').write_text('typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define taskENTER_CRITICAL(g) ((void)(g))\n#define taskEXIT_CRITICAL(g) ((void)(g))\n')
   (folder/'freertos/task.h').write_text('typedef void *TaskHandle_t;\n')
   code=BOUNDARY+(component/'xz_codec_memory.c').read_text()+stubs+function(source,'reconnect_status')+'static void run_startup(void){cJSON *cloud=NULL;bool codec_memory_owned=false,warm=false,online=false,*wifi_up=&online;\n'+startup+'(void)cloud;(void)cloud_cached;finish:\n'+end+'}\n'+cases
   src,exe=folder/'lifecycle.c',folder/'lifecycle';src.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Werror','-fsanitize=undefined',
    '-I',str(folder),'-I',str(component),'-I',str(component/'include'),'-I',str(ROOT/'components/core/ports/include'),str(src),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)

if __name__=='__main__':unittest.main()
