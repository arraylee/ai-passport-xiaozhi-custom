"""Execute production AT handlers against RAM-only config/identity/radio fakes.

No serial ports, device, NVS, files with identity data, or actual reboot are used.
The default is ASAN+UBSan; PASSPORT_HOST_SANITIZERS=undefined explicitly selects
UBSan-only behavior checks when the host ASAN runtime cannot start.
"""
import os
from pathlib import Path
import tempfile
import unittest

from test_ble_advertising import function
from test_clicker_platform import host_sanitizers, run_host

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "services/profile_ctl.h"
#include "hal/hal_identity.h"
#include "hal/hal_kv.h"
#define PERIPH_DISPLAY 1
#define PERIPH_AUDIO 1
#define JPEG_SLOT_AVATAR 0
#define JPEG_SLOT_FULL 1
#define BOOT_RTTTL_KV_KEY "boot_rtttl"
#define pdMS_TO_TICKS(ms) (ms)
static profile_data_t s_profile, persisted;
static atomic_bool s_jpeg_upload_active;
static bool decode_busy, capture_busy, save_ok=true, erase_ok=true, image_ok=true;
static unsigned saves, audio_sets, erased, images, rebooted, delay_ms, activity, acknowledgements, flushes, identity_reads;
static unsigned voice_stops,voice_interrupts,natural_sets;
static bool natural,voice_running,natural_save_fail;
enum {XZ_READY,XZ_LISTEN,XZ_SPEAK,XZ_THINK,XZ_STANDBY};
typedef struct {bool natural_interrupt; int phase;} xz_view_t;
static void xiaozhi_talk(void) {}
static void xiaozhi_resume(void) {}
static void xiaozhi_interrupt(void){voice_interrupts++;}
static bool xiaozhi_stopped(void){return !voice_running;}
static void xiaozhi_snapshot(xz_view_t*v){v->natural_interrupt=natural;v->phase=XZ_LISTEN;}
static bool xiaozhi_set_natural_interrupt(bool enabled){natural_sets++;if(natural_save_fail)return false;natural=enabled;return true;}
static void xiaozhi_stop(void) { ++voice_stops; }
static unsigned test_entries; static bool test_voice;
static int at_test_entry(bool voice,char*out,int n){test_entries++;test_voice=voice;return snprintf(out,n,"+OK test_entry");}
static int s_au, last_volume;
static void *s_disp=(void*)1;
static unsigned brightness_applied=100;
static void hal_display_set_backlight(void*d,uint8_t value){assert(d==s_disp);brightness_applied=value;}
static uint8_t identity_ram[8]={0xa1,0x22,0x34,0x46,0x58,0x6a,0x7c,0x8e};
static uint8_t wifi_ram[8]={0xb2,0x23,0x35,0x47,0x59,0x6b,0x7d,0x8f};
static char reply[512];
static size_t reply_capacity=sizeof reply;
static bool jpeg_view_decode_busy(void) { return decode_busy; }
static bool capture_ui_blocked(void) { return capture_busy; }
static bool save_profile(void) { ++saves; if(save_ok)persisted=s_profile; return save_ok; }
static int erase(hal_kv_t *self,const char *key) {
    (void)self; assert(strcmp(key,BOOT_RTTTL_KV_KEY)==0); ++erased; return erase_ok ? 0 : -1;
}
static const hal_kv_api_t kv_api={.erase=erase};
static hal_kv_t kv={.api=&kv_api};
static hal_kv_t *s_kv=&kv;
static int jpeg_store_clear(int slot) { assert(slot==JPEG_SLOT_AVATAR||slot==JPEG_SLOT_FULL); ++images; return image_ok?0:-1; }
static void audio_service_set_volume(void *audio,int volume) { assert(audio==&s_au); ++audio_sets; last_volume=volume; }
static void note_activity(void) { ++activity; }
static void vTaskDelay(int ticks) { assert(acknowledgements==1 && flushes==1); delay_ms+=(unsigned)ticks; }
static void esp_restart(void) { assert(acknowledgements==1 && flushes==1 && delay_ms==150); ++rebooted; }
static size_t ram_fwrite(const void *ptr,size_t size,size_t count,FILE *stream) {
    assert(ptr==reply && size==1 && count<=reply_capacity && stream==stdout);
    ++acknowledgements; return count;
}
static int ram_fflush(FILE *stream) { assert(stream==stdout); ++flushes; return 0; }
#define fwrite ram_fwrite
#define fflush ram_fflush
static int get_sn(hal_identity_t *self,char *out,int cap) { (void)self; ++identity_reads; return snprintf(out,cap,"TEST00000001"); }
static int get_key(hal_identity_t *self,char *out,int cap) { (void)self; ++identity_reads; return snprintf(out,cap,"SYNTHETIC_RAM_ONLY"); }
static int get_hw(hal_identity_t *self,char *out,int cap) { (void)self; ++identity_reads; return snprintf(out,cap,"host-only"); }
static bool provisioned(hal_identity_t *self) { (void)self; return true; }
static const hal_identity_api_t id_api={.get_sn=get_sn,.get_key=get_key,.get_hw_ver=get_hw,.is_provisioned=provisioned};
static hal_identity_t identity={.api=&id_api};
static hal_identity_t *s_id=&identity;
'''
CASES = r'''
int main(int argc,char **argv) {
    fputs("PASSPORT_AT_MAIN_ENTERED\n",stderr); fflush(stderr);
    assert(argc>=3);
    fprintf(stderr,"PASSPORT_AT_CASE %s\n",argv[1]); fflush(stderr);
    profile_ctl_init(&s_profile);
    strcpy(s_profile.name,"custom"); strcpy(s_profile.subtitle,"custom subtitle");
    strcpy(s_profile.avatar_name,"other"); s_profile.img_mode=IMG_MODE_FULLSCREEN;
    s_profile.volume=31; s_profile.sleep_min=9; s_profile.token=1234;
    s_profile.token_max=5678; s_profile.level=4; s_profile.game_total=345; s_profile.game_best=67;
    profile_data_t before=s_profile; persisted=before;
    uint8_t identity_before[8],wifi_before[8]; memcpy(identity_before,identity_ram,8); memcpy(wifi_before,wifi_ram,8);
    const char *mode=argv[1],*command=argv[2];
    if(!strcmp(mode,"natural_busy")) voice_running=true;
    if(!strcmp(mode,"natural_fail")) natural_save_fail=true;
    if(!strcmp(mode,"busy_decode")) decode_busy=true;
    if(!strcmp(mode,"busy_upload")) atomic_store(&s_jpeg_upload_active,true);
    if(!strcmp(mode,"busy_capture")) capture_busy=true;
    if(!strcmp(mode,"fail_profile")||!strcmp(mode,"fail_config")) save_ok=false;
    if(!strcmp(mode,"fail_audio")) erase_ok=false;
    if(!strcmp(mode,"fail_images")) image_ok=false;
    if(!strcmp(mode,"missing_kv")) s_kv=NULL;
    if(!strcmp(mode,"small_reboot")||!strcmp(mode,"small_reset")||!strcmp(mode,"small_query")) reply_capacity=4;
    const char embedded[]="at+reboot\0junk";
    int command_len=(int)strlen(command);
    if(!strcmp(mode,"embedded")) { command=embedded; command_len=(int)sizeof embedded-1; }
    int rc=on_at_command(command,command_len,reply,(int)reply_capacity,NULL);
    assert(memcmp(identity_before,identity_ram,8)==0 && memcmp(wifi_before,wifi_ram,8)==0);
    if(!strcmp(mode,"voice_interrupt")) {
        assert(voice_interrupts==1&&!voice_stops&&!saves&&!audio_sets&&!rebooted);
        assert(!strncmp(reply,"+OK voice_interrupt_requested",29));
    } else if(!strcmp(mode,"natural_set")) {
        assert(natural_sets==1&&natural==(bool)atoi(argv[3])&&!saves&&!audio_sets);
        assert(!strncmp(reply,"+OK natural_interrupt=",22));
    } else if(!strcmp(mode,"natural_query")) {
        assert(!natural_sets&&!saves&&!strcmp(reply,"+OK natural_interrupt=0\r\n"));
    } else if(!strcmp(mode,"natural_busy")||!strcmp(mode,"natural_fail")) {
        assert(!natural&&!saves&&!audio_sets&&!strncmp(reply,"+ERR=",5));
        assert(natural_sets==(!strcmp(mode,"natural_fail")?1U:0U));
    } else if(!strcmp(mode,"test_entry")) {
        assert(test_entries==1 && test_voice==(bool)atoi(argv[3]) && !saves && !rebooted && !voice_stops);
    } else if(!strcmp(mode,"voice_stop")) {
        assert(voice_stops==1 && !rebooted && !saves && !audio_sets && !images && !erased);
        assert(!strncmp(reply,"+OK voice_stop_requested",24));
        assert(memcmp(&before,&s_profile,sizeof before)==0);
    } else if(!strcmp(mode,"small_query")) {
        assert(rc==3 && reply[3]==0 && !saves && !rebooted);
    } else if(!strcmp(mode,"query")) {
        assert(rc>0 && !strncmp(reply,"+CONFIG:",8) && strstr(reply,"volume=31,standby_time=540"));
        assert(identity_reads==3 && !saves && !audio_sets && !erased && !images && !rebooted);
    } else if(!strcmp(mode,"help")) {
        assert(rc>0 && !strncmp(reply,"+COMMAND:",9) && !saves && !rebooted && !identity_reads);
    } else if(!strcmp(mode,"volume")) {
        int wanted=atoi(argv[3]); assert(rc>0 && !strncmp(reply,"+OK volume=",11));
        assert(s_profile.volume==wanted && persisted.volume==wanted && saves==1 && audio_sets==1 && last_volume==wanted);
        before.volume=wanted; assert(memcmp(&before,&s_profile,sizeof before)==0);
        assert(!rebooted && !erased && !images && !activity);
    } else if(!strcmp(mode,"standby")) {
        int wanted=atoi(argv[3]); assert(rc>0 && !strncmp(reply,"+OK standby_time=",17));
        assert(s_profile.sleep_min==wanted && persisted.sleep_min==wanted && saves==1 && activity==1);
        before.sleep_min=wanted; assert(memcmp(&before,&s_profile,sizeof before)==0);
        assert(!rebooted && !erased && !images && !audio_sets);
    } else if(!strcmp(mode,"reboot")||!strcmp(mode,"small_reboot")) {
        assert(rc==0 && rebooted==1 && delay_ms==150 && acknowledgements==1);
        assert(!saves && !erased && !images && !audio_sets && !identity_reads);
        assert(memcmp(&before,&s_profile,sizeof before)==0);
    } else if(!strcmp(mode,"reset")||!strcmp(mode,"small_reset")) {
        profile_data_t defaults;profile_ctl_init(&defaults);
        memcpy(before.name,defaults.name,sizeof before.name);memcpy(before.subtitle,defaults.subtitle,sizeof before.subtitle);
        memcpy(before.avatar_name,defaults.avatar_name,sizeof before.avatar_name);before.img_mode=defaults.img_mode;
        assert(rc==0 && rebooted==1 && saves==1 && erased==1 && images==2);
        assert(memcmp(&before,&s_profile,sizeof before)==0 && memcmp(&before,&persisted,sizeof before)==0);
        assert(!identity_reads && !audio_sets);
    } else if(!strcmp(mode,"fail_config")) {
        assert(rc>0 && !strncmp(reply,"+ERR=save",9) && saves==1 && !rebooted);
        assert(memcmp(&persisted,&before,sizeof before)==0);
        assert(memcmp(&s_profile,&before,sizeof before)==0 && !audio_sets && !activity);
    } else if(!strncmp(mode,"fail_",5)||!strcmp(mode,"missing_kv")) {
        assert(rc>0 && !strncmp(reply,"+ERR=binding_reset",18) && !rebooted && !delay_ms && !acknowledgements);
        assert(!identity_reads && !audio_sets);
    } else {
        assert(!voice_stops);
        assert(rc>0 && !strncmp(reply,"+ERR=",5));
        assert(!saves && !audio_sets && !erased && !images && !rebooted && !activity && !delay_ms && !acknowledgements);
        assert(memcmp(&s_profile,&before,sizeof before)==0 && memcmp(&persisted,&before,sizeof before)==0);
    }
    return 0;
}
'''
# Harness output markers must use the real stdio flush before handler-only fakes.
CASES = CASES.replace('int main(int argc,char **argv) {', '#undef fflush\nint main(int argc,char **argv) {')


class AtCommandHandlers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sanitizers = host_sanitizers()
        cls.directory = tempfile.TemporaryDirectory(prefix="passport-at-ram-")
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        app = (ROOT / "components/app/src/app.c").read_text()
        source = directory / "test.c"
        source.write_text(BOUNDARY + "\n".join(function(app,name) for name in
                          ("plays_get_brightness","plays_preview_brightness","plays_set_brightness","plays_get_sleep_minutes","at_after","at_is","at_decimal","at_reply","at_reboot","at_binding_reset","on_at_command")) + CASES)
        cls.executable = directory / "test"
        run_host(["cc","-std=c11","-UNDEBUG","-Wall","-Wextra","-Werror",f"-fsanitize={cls.sanitizers}",
                  "-fno-sanitize-recover=all","-g","-I"+str(ROOT/"components/core/services/include"),
                  "-I"+str(ROOT/"components/core/ports/include"),str(source),
                  str(ROOT/"components/core/services/src/profile_ctl.c"),"-o",str(cls.executable)],
                 timeout=30,phase="AT RAM fixture compile",sanitizers=cls.sanitizers)

    def command(self,mode,command,*args):
        environment=dict(os.environ)
        environment["ASAN_OPTIONS"]=environment.get("ASAN_OPTIONS","")+":detect_leaks=0"
        run_host([str(self.executable),mode,command,*map(str,args)],timeout=5,
                 phase=f"AT {mode} {command!r}",sanitizers=self.sanitizers,check_main=True,env=environment,
                 main_marker="PASSPORT_AT_MAIN_ENTERED",case_marker="PASSPORT_AT_CASE ")

    def test_query_and_help_do_not_write(self):
        self.command("query","at+config=?")
        self.command("help","AT+COMMAND=?")

    def test_voice_stop_requests_cancellation_without_reboot_or_settings_write(self):
        self.command("voice_stop","AT+COMMAND=voice,stop")
        for command in ("AT+COMMAND=voice,stopX", "AT+COMMAND=voice,start"):
            self.command("invalid",command)

    def test_interruption_and_preference_console_boundaries(self):
        self.command("voice_interrupt","AT+COMMAND=voice,interrupt")
        self.command("natural_set","AT+CONFIG=common,natural_interrupt,0",0)
        self.command("natural_set","AT+CONFIG=common,natural_interrupt,1",1)
        self.command("natural_query","AT+CONFIG=common,natural_interrupt,?")
        self.command("natural_busy","AT+CONFIG=common,natural_interrupt,1")
        self.command("natural_fail","AT+CONFIG=common,natural_interrupt,1")
        for value in ("2","-1","true","1x",""):
            self.command("invalid","AT+CONFIG=common,natural_interrupt,"+value)

    def test_only_exact_local_fixture_entry_commands(self):
        self.command("test_entry","AT+COMMAND=voice,check",1)
        self.command("test_entry","AT+COMMAND=test,home",0)
        for command in ("AT+COMMAND=voice,checkX", "AT+COMMAND=test,homeX"):
            self.command("invalid",command)

    def test_valid_volume_endpoints_and_case_whitespace(self):
        for value in (0,1,50,99,100):
            with self.subTest(value=value): self.command("volume",f" \tAT+CONFIG=COMMON,VOLUME,{value}\r\n",value)

    def test_invalid_volume_does_not_write(self):
        for value in ("-1","101","","abc","10junk","0x10","1.5","999999999999999999999999"):
            with self.subTest(value=value): self.command("invalid",f"at+config=common,volume,{value}")

    def test_standby_rounding_zero_and_clamp(self):
        for seconds,minutes in ((0,0),(29,0),(30,1),(59,1),(60,1),(89,1),(90,2),(600,10),(86400,1440),(90000,1440),(2147483647,1440)):
            with self.subTest(seconds=seconds): self.command("standby",f"at+config=common,standby_time,{seconds}",minutes)

    def test_invalid_standby_does_not_write(self):
        for value in ("-1","","abc","10junk","1.5","999999999999999999999999"):
            with self.subTest(value=value): self.command("invalid",f"at+config=common,standby_time,{value}")

    def test_unknown_group_key_and_command_do_not_write(self):
        for command in ("at+config=other,volume,10","at+config=common,other,10","at+command=unknown","garbage"):
            with self.subTest(command=command): self.command("invalid",command)

    def test_only_exact_reboot_commands_authorize_restart(self):
        for command in ("at+reboot","AT+COMMAND=restart","at+command=restart,now"):
            with self.subTest(command=command): self.command("reboot",command)

    def test_reboot_suffixes_do_not_authorize_restart(self):
        for command in ("at+rebootX","at+command=restartX","at+command=restart,later","at+reboot"+"x"*200):
            with self.subTest(command=command): self.command("invalid",command)

    def test_embedded_nul_does_not_authorize_prefix(self):
        self.command("embedded","unused")

    def test_small_query_returns_only_bytes_present_in_output(self):
        self.command("small_query","at+config=?")

    def test_binding_reset_preserves_identity_wifi_scores_and_other_settings(self):
        self.command("reset","at+command=binding,reset")

    def test_binding_reset_suffix_does_not_authorize_erasure(self):
        self.command("invalid","at+command=binding,resetX")

    def test_binding_reset_busy_does_not_mutate_or_reboot(self):
        for mode in ("busy_decode","busy_upload","busy_capture"):
            with self.subTest(mode=mode): self.command(mode,"at+command=binding,reset")

    def test_binding_reset_storage_failures_never_reboot(self):
        for mode in ("fail_profile","fail_audio","fail_images","missing_kv"):
            with self.subTest(mode=mode): self.command(mode,"at+command=binding,reset")

    def test_config_save_failure_is_reported_without_persisting(self):
        self.command("fail_config","at+config=common,volume,40")
        self.command("fail_config","at+config=common,standby_time,600")

    def test_reboot_response_never_reads_beyond_supplied_output_capacity(self):
        self.command("small_reboot","at+reboot")

    def test_reset_response_never_reads_beyond_supplied_output_capacity(self):
        self.command("small_reset","at+command=binding,reset")


if __name__ == "__main__":
    unittest.main()
