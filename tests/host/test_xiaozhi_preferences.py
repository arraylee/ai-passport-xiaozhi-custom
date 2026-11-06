"""Production preference persistence and volume rollback at storage boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function

ROOT = Path(__file__).resolve().parents[2]


def run_c(source):
    with tempfile.TemporaryDirectory() as directory:
        path, exe = Path(directory) / 'preferences.c', Path(directory) / 'preferences'
        path.write_text(source)
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', str(path), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, timeout=5)


class XiaozhiPreferencesTest(unittest.TestCase):
    def test_button_default_persistence_and_failed_commit(self):
        source = (ROOT / 'components/platform/xiaozhi/xiaozhi.c').read_text()
        boundary = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#define ESP_OK 0
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
typedef int nvs_handle_t;
typedef struct {unsigned (*get_volume)(void*);bool (*set_volume)(void*,unsigned);void *user;} xz_config_t;
typedef struct {unsigned avatar,volume;bool natural_interrupt,output_active;} xz_view_t;
static bool xiaozhi_output_active(void){return false;}
static xz_config_t config;
static xz_view_t view;
static atomic_uint avatar;
static atomic_bool natural_interrupt;
static unsigned writes,commits,volume_calls,applied_volume;
static bool missing=true,open_fail,set_fail,commit_fail,volume_fail;
static uint8_t disk_natural,staged;
static void allocation_failure_init(void){}
static int nvs_open(const char*n,int mode,nvs_handle_t*h){assert(!strcmp(n,"xiaozhi"));(void)mode;*h=1;return open_fail?-1:0;}
static void nvs_close(nvs_handle_t h){assert(h==1);}
static int nvs_get_u8(nvs_handle_t h,const char*k,uint8_t*out){assert(h==1);if(!strcmp(k,"avatar")){*out=8;return 0;}assert(!strcmp(k,"natural"));if(missing)return -1;*out=disk_natural;return 0;}
static int nvs_set_u8(nvs_handle_t h,const char*k,uint8_t value){assert(h==1&&!strcmp(k,"natural"));writes++;staged=value;return set_fail?-1:0;}
static int nvs_commit(nvs_handle_t h){assert(h==1);commits++;if(commit_fail)return -1;disk_natural=staged;missing=false;return 0;}
static unsigned volume_get(void*u){assert(u==(void*)1);return applied_volume;}
static bool volume_set(void*u,unsigned value){assert(u==(void*)1);volume_calls++;if(volume_fail)return false;applied_volume=value;return true;}
'''
        cases = r'''
int main(void){
 xz_config_t c={0};xz_view_t out;xiaozhi_init(&c);xiaozhi_snapshot(&out);
 assert(!out.natural_interrupt&&out.avatar==0&&out.volume==95);
 assert(xiaozhi_set_natural_interrupt(false)&&writes==0);
 open_fail=true;assert(!xiaozhi_set_natural_interrupt(true)&&writes==0);open_fail=false;
 set_fail=true;assert(!xiaozhi_set_natural_interrupt(true)&&commits==0);set_fail=false;
 commit_fail=true;assert(!xiaozhi_set_natural_interrupt(true));xiaozhi_snapshot(&out);assert(!out.natural_interrupt);commit_fail=false;
 assert(xiaozhi_set_natural_interrupt(true));xiaozhi_init(&c);xiaozhi_snapshot(&out);assert(out.natural_interrupt);
 assert(xiaozhi_set_natural_interrupt(false));xiaozhi_init(&c);xiaozhi_snapshot(&out);assert(!out.natural_interrupt);
 disk_natural=9;xiaozhi_init(&c);xiaozhi_snapshot(&out);assert(!out.natural_interrupt);
 assert(!xiaozhi_set_volume(60));c=(xz_config_t){volume_get,volume_set,(void*)1};xiaozhi_init(&c);
 assert(!xiaozhi_set_volume(101)&&volume_calls==0);
 assert(xiaozhi_set_volume(60));xiaozhi_snapshot(&out);assert(out.volume==60);
 volume_fail=true;assert(!xiaozhi_set_volume(90));xiaozhi_snapshot(&out);assert(out.volume==60);
 return 0;
}
'''
        funcs = '\n'.join(function(source, name) for name in
                          ['xiaozhi_snapshot', 'xiaozhi_init',
                           'xiaozhi_set_natural_interrupt', 'xiaozhi_set_volume'])
        run_c(boundary + funcs + cases)

    def test_profile_volume_save_failure_restores_previous_setting(self):
        source = (ROOT / 'components/app/src/app.c').read_text()
        boundary = r'''
#include <assert.h>
#include <stdbool.h>
static struct {unsigned volume;} s_profile={95};
static int s_au;
static unsigned writes,applies,effective=95;
static bool fail_save;
static bool save_profile(void){writes++;return !fail_save;}
static void audio_service_set_volume(int*a,unsigned volume){assert(a==&s_au);applies++;effective=volume;}
'''
        cases = r'''
int main(void){
 assert(plays_get_volume(0)==95);
 assert(!plays_set_volume(0,101)&&writes==0&&applies==0);
 fail_save=true;assert(!plays_set_volume(0,60));assert(plays_get_volume(0)==95&&effective==95&&applies==0);
 fail_save=false;assert(plays_set_volume(0,60));assert(plays_get_volume(0)==60&&effective==60&&applies==1);
 unsigned before=writes;assert(plays_set_volume(0,60)&&writes==before&&applies==1);
 assert(plays_set_volume(0,0)&&effective==0);assert(plays_set_volume(0,100)&&effective==100);
 return 0;
}
'''
        run_c(boundary + function(source, 'plays_get_volume') +
              function(source, 'plays_set_volume') + cases)


if __name__ == '__main__':
    unittest.main()
