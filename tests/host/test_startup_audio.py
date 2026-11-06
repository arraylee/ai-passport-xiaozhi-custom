"""Link the two production audio entry points against distinct embedded assets."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
R=Path(__file__).resolve().parents[2]
class StartupAudio(unittest.TestCase):
 def test_startup_and_welcome_keep_distinct_linker_symbols(self):
  s=(R/'components/platform/xiaozhi/xiaozhi.c').read_text()
  boundary=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#define pdMS_TO_TICKS(x) (x)
#define XZ_WELCOME 1
typedef void (*xz_status_fn)(int,const char*);
static int config;
static atomic_bool welcome_visible=true,setup_confirmed;
static bool running;
static bool xiaozhi_stopped(void){return !running;}
static void vTaskDelay(unsigned n){(void)n;assert(0);}
static size_t xz_play_prompt(void*c,const atomic_bool*a,const atomic_bool*b,const uint8_t*p,size_t n){
 assert(c==&config&&!atomic_load(a)&&!atomic_load(b)&&n==25251&&p[0]==0x7d&&p[n-1]==0x7d);return n*2;
}
static size_t xz_play_pcm_prompt(void*c,const atomic_bool*a,const atomic_bool*b,const uint8_t*p,size_t n){
 assert(c==&config&&!atomic_load(a)&&!atomic_load(b)&&n==80&&p[0]==0x31&&p[n-1]==0x31);atomic_store(&setup_confirmed,true);return n/2;
}
static void publish(int phase,const char*s){assert(phase==1&&!s[0]);}
__asm__(".data\n.globl _binary_welcome_zh_pcm_start\n_binary_welcome_zh_pcm_start:\n.space 80,0x31\n.globl _binary_welcome_zh_pcm_end\n_binary_welcome_zh_pcm_end:\n.globl _binary_startup_magicbox_adpcm_start\n_binary_startup_magicbox_adpcm_start:\n.space 25251,0x7d\n.globl _binary_startup_magicbox_adpcm_end\n_binary_startup_magicbox_adpcm_end:\n");
'''
  code=boundary+function(s,'xz_wait_for_setup')+'\n'+function(s,'xiaozhi_play_startup_sound')+r'''
int main(void){atomic_bool stop=false;assert(xiaozhi_play_startup_sound()==50502);assert(xz_wait_for_setup(&stop,publish));running=true;assert(!xiaozhi_play_startup_sound());return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(code)
   subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
