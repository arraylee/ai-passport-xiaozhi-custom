"""Run actual on_cfg_message/save_profile with real parsers and RAM-only HAL boundaries.

No serial/BLE, persistent files with user data, audio playback, or device operations.
This focused lane uses UBSan explicitly; it does not claim ASan or hardware coverage.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "services/profile_ctl.h"
#include "services/config_json.h"
#include "services/score_rx.h"
#include "services/rtttl.h"
#include "services/jpeg_rx.h"
#include "services/screen_capture.h"
#include "hal/hal_config.h"
#include "hal/hal_kv.h"
#define PERIPH_DISPLAY 1
#define PERIPH_AUDIO 1
#define TAG "synthetic"
#define BOOT_RTTTL_KV_KEY "boot_rtttl"
#define JPEG_SLOT_AVATAR 0
#define JPEG_SLOT_FULL 1
#define JPEG_VIEW_AVATAR 0
#define JPEG_VIEW_FULLSCREEN 1
#define PROFILE_UI_REFRESH_NAME 1u
#define PROFILE_UI_REFRESH_TITLE 2u
#define PROFILE_UI_REFRESH_INTRO 4u
static void quiet(const char *tag,const char *fmt,...) {(void)tag;(void)fmt;}
#define ESP_LOGI(...) quiet(__VA_ARGS__)
#define ESP_LOGD(...) quiet(__VA_ARGS__)
#define ESP_LOGW(...) quiet(__VA_ARGS__)
#define ESP_LOGE(...) quiet(__VA_ARGS__)
static profile_data_t s_profile;
static atomic_bool s_sleep_preparing,s_factory_command_active,s_capture_active,s_capture_start_pending,s_jpeg_upload_active;
static atomic_uint s_profile_ui_pending;
static unsigned activity,notices,profile_writes,music_writes,queued,volume_sets,view_modes,view_avatars,flushes,begin_calls,write_calls,end_calls,clock_sets,capture_controls,score_publishes;
static uint8_t last_type,last_status,profile_blob[PROFILE_BLOB_SIZE],music_blob[1024];
static int profile_len,music_len,last_volume,last_view,last_slot,s_au;
static unsigned applied_ui;
static bool kv_fail,decode_busy,avatar_available=true;
static char requested_avatar[16];
static int kv_set(hal_kv_t *self,const char *key,const void *data,int n){
 (void)self;
 if(!strcmp(key,"profile")){profile_writes++;if(kv_fail)return -1;assert(n<=PROFILE_BLOB_SIZE);memcpy(profile_blob,data,(size_t)n);profile_len=n;}
 else {assert(!strcmp(key,BOOT_RTTTL_KV_KEY));music_writes++;if(kv_fail)return -1;assert(n<=1024);memcpy(music_blob,data,(size_t)n);music_len=n;}
 return 0;
}
static const hal_kv_api_t api={.set=kv_set};static hal_kv_t kv={.api=&api},*s_kv=&kv;
static score_rx_t s_score_rx;static uint8_t s_score_rx_buf[1024],s_score_play_buf[1024];
static int s_score_play_len=3,s_music_interrupt;static void *s_music_sig=(void*)1;
static jpeg_rx_t s_jpeg_rx;
static int xSemaphoreGive(void *sig){assert(sig==(void*)1);assert(music_writes==1&&music_len>0);assert(!memcmp(s_score_play_buf,music_blob,(size_t)music_len));assert(s_score_play_len==music_len&&s_music_interrupt==1);queued++;return 1;}
static void notify_status(uint8_t type,uint8_t status){last_type=type;last_status=status;notices++;}
static void note_activity(void){activity++;}
static bool jpeg_view_decode_busy(void){return decode_busy;}
static int jpeg_store_begin(int slot,uint32_t total){assert(total==4);last_slot=slot;begin_calls++;return 0;}
static int jpeg_store_write(const uint8_t *data,int n){assert(data&&n==4);write_calls++;return 0;}
static int jpeg_store_end(void){end_calls++;return 0;}
static void jpeg_view_request_mode(int mode){last_view=mode;view_modes++;}
static void jpeg_view_request_avatar(const char *name){assert(profile_len>0);snprintf(requested_avatar,sizeof requested_avatar,"%s",name);view_avatars++;}
static bool avatar_store_has(const char *name){assert(name);return avatar_available;}
static void audio_service_set_volume(void *audio,int v){assert(audio==&s_au);last_volume=v;volume_sets++;}
static void publish_gamescore(void){score_publishes++;}
static void profile_ui_flush_pending(void){applied_ui|=atomic_exchange(&s_profile_ui_pending,0);flushes++;}
static int fake_settimeofday(const struct timeval *tv,void *tz){(void)tz;assert(tv->tv_sec==1700000000&&tv->tv_usec==0);clock_sets++;return 0;}
#define settimeofday fake_settimeofday
static void on_capture_control(const sc_control_t *c){assert(c);capture_controls++;}
'''
CASES = r'''
static void send(uint8_t type,const void *p,int n){unsigned before=notices;on_cfg_message(type,p,n,NULL);assert(notices==before+1);assert(last_type==type);}
static void json(const char *s){send(CFG_MSG_JSON,s,(int)strlen(s));}
static void score(const char *s){size_t n=strlen(s);uint8_t start[]={0,(uint8_t)n,(uint8_t)(n>>8)},data[1025]={1},end[]={2};memcpy(data+1,s,n);send(CFG_MSG_SCORE,start,3);assert(last_status==CFG_ST_ACK);send(CFG_MSG_SCORE,data,(int)n+1);assert(last_status==CFG_ST_ACK);send(CFG_MSG_SCORE,end,1);}
static void jpeg(void){uint8_t start[]={0,4,0,0,0},data[]={1,1,2,3,4},end[]={2};send(CFG_MSG_JPEG,start,5);assert(last_status==CFG_ST_ACK&&atomic_load(&s_jpeg_upload_active));send(CFG_MSG_JPEG,data,5);assert(last_status==CFG_ST_ACK);send(CFG_MSG_JPEG,end,1);assert(last_status==CFG_ST_ACK&&!atomic_load(&s_jpeg_upload_active));}
int main(int argc,char **argv){
 assert(argc==2);int mode=atoi(argv[1]);profile_ctl_init(&s_profile);strcpy(s_profile.avatar_name,"old");s_profile.volume=31;s_profile.sleep_min=9;s_profile.game_total=123;s_profile.game_best=45;
 assert(profile_serialize(&s_profile,profile_blob,sizeof profile_blob)>0);profile_len=PROFILE_BLOB_SIZE;profile_data_t before=s_profile;
 memcpy(music_blob,"old",3);music_len=3;memcpy(s_score_play_buf,"old",3);score_rx_init(&s_score_rx,s_score_rx_buf,sizeof s_score_rx_buf);jpeg_rx_init(&s_jpeg_rx);
 if(mode==0){for(unsigned type=4;type<=5;type++){send((uint8_t)type,"x",1);assert(last_status==CFG_ST_ERR_UNSUPPORTED);}send(0xff,"x",1);assert(last_status==CFG_ST_ERR_UNSUPPORTED);json("{\"pet_enabled\":true,\"pet_type\":2}");assert(last_status==CFG_ST_ERR_NO_FIELD&&!profile_writes&&!music_writes&&!queued);assert(!memcmp(&s_profile,&before,sizeof before));}
 else if(mode==1||mode==2){if(mode==1)atomic_store(&s_sleep_preparing,true);else atomic_store(&s_factory_command_active,true);json("{\"volume\":88}");assert(last_status==CFG_ST_ERR_BUSY);uint8_t start[]={0,4,0,0,0};send(CFG_MSG_JPEG,start,5);assert(last_status==CFG_ST_ERR_BUSY);assert(!activity&&!profile_writes&&!music_writes&&!queued&&!begin_calls&&!volume_sets);assert(!memcmp(&s_profile,&before,sizeof before));}
 else if(mode==3){score("not RTTTL");assert(last_status==CFG_ST_ERR_PARSE&&!music_writes&&!queued&&!s_music_interrupt);assert(s_score_play_len==3&&!memcmp(s_score_play_buf,"old",3));}
 else if(mode==4||mode==5||mode==6){if(mode==4)kv_fail=true;if(mode==5)s_kv=NULL;if(mode==6)s_music_sig=NULL;score("qa:d=4,o=5,b=120:c,d,e");assert(last_status==CFG_ST_ERR_STORAGE&&!queued&&!s_music_interrupt);assert(music_writes==(mode==4?1u:0u));assert(music_len==3&&!memcmp(music_blob,"old",3));assert(s_score_play_len==3&&!memcmp(s_score_play_buf,"old",3));}
 else if(mode==7){score("qa:d=4,o=5,b=120:c,d,e");assert(last_status==CFG_ST_DONE&&music_writes==1&&queued==1);uint8_t end[]={2};send(CFG_MSG_SCORE,end,1);assert(last_status==CFG_ST_ERR_SEQ&&queued==1&&music_writes==1);assert(!profile_writes);}
 else if(mode>=8&&mode<=10){if(mode==8)decode_busy=true;if(mode==9)atomic_store(&s_capture_active,true);if(mode==10)atomic_store(&s_capture_start_pending,true);uint8_t start[]={0,4,0,0,0};send(CFG_MSG_JPEG,start,5);assert(last_status==CFG_ST_ERR_BUSY&&!begin_calls&&!write_calls&&!end_calls&&!view_modes&&!profile_writes);assert(!s_jpeg_rx.receiving&&!atomic_load(&s_jpeg_upload_active));}
 else if(mode==11||mode==12){s_profile.img_mode=mode==11?IMG_MODE_AVATAR:IMG_MODE_FULLSCREEN;jpeg();assert(begin_calls==1&&write_calls==1&&end_calls==1&&view_modes==1);assert(last_slot==(mode==11?JPEG_SLOT_AVATAR:JPEG_SLOT_FULL));assert(last_view==(mode==11?JPEG_VIEW_AVATAR:JPEG_VIEW_FULLSCREEN));assert(profile_writes==(mode==11?1u:0u));if(mode==11){profile_data_t saved;assert(profile_deserialize(&saved,profile_blob,profile_len)==0&&!saved.avatar_name[0]);}}
 else if(mode==13){json("{\"nickname\":\"Synthetic QA\",\"title\":\"Tester\",\"intro\":\"RAM only\",\"volume\":77,\"sleep_min\":3,\"avatar_name\":\"valid\"}");assert(last_status==CFG_ST_DONE&&profile_writes==1&&volume_sets==1&&last_volume==77&&view_avatars==1);profile_data_t saved;assert(profile_deserialize(&saved,profile_blob,profile_len)==0);assert(!strcmp(saved.name,"Synthetic QA")&&!strcmp(saved.role,"Tester")&&!strcmp(saved.subtitle,"RAM only")&&saved.volume==77&&saved.sleep_min==3&&!strcmp(saved.avatar_name,"valid"));assert(applied_ui==7&&!strcmp(requested_avatar,"valid"));}
 else if(mode==14){kv_fail=true;json("{\"nickname\":\"Not committed\",\"avatar_name\":\"valid\"}");assert(last_status==CFG_ST_ERR_STORAGE&&profile_writes==1&&!view_avatars&&!applied_ui&&!flushes);assert(!memcmp(&s_profile,&before,sizeof before));profile_data_t saved;assert(profile_deserialize(&saved,profile_blob,profile_len)==0&&!memcmp(&saved,&before,sizeof before));}
 else if(mode==15){json("{\"time\":1700000000}");assert(last_status==CFG_ST_DONE&&clock_sets==1&&!profile_writes&&!volume_sets&&!view_avatars);}
 else if(mode==16){avatar_available=false;json("{\"avatar_name\":\"missing\"}");assert(last_status==CFG_ST_ERR_AVATAR&&!profile_writes&&!view_avatars&&!strcmp(s_profile.avatar_name,"old"));}
 else if(mode==17){json("{\"nickname\":\"bad\",\"volume\":1e999}");assert(last_status==CFG_ST_ERR_PARSE&&!profile_writes&&!volume_sets&&!view_avatars&&!memcmp(&s_profile,&before,sizeof before));}
 else if(mode==18||mode==19){kv_fail=mode==18;json("{\"nickname\":\"New\",\"volume\":77,\"sleep_min\":3,\"game_clear\":true,\"time\":1700000000}");assert(profile_writes==1);if(kv_fail){assert(last_status==CFG_ST_ERR_STORAGE&&!volume_sets&&!clock_sets&&!score_publishes&&!applied_ui&&!flushes);assert(!memcmp(&s_profile,&before,sizeof before));profile_data_t saved;assert(profile_deserialize(&saved,profile_blob,profile_len)==0&&!memcmp(&saved,&before,sizeof before));}else{assert(last_status==CFG_ST_DONE&&volume_sets==1&&last_volume==77&&clock_sets==1&&score_publishes==1);profile_data_t saved;assert(profile_deserialize(&saved,profile_blob,profile_len)==0&&saved.volume==77&&saved.sleep_min==3&&saved.game_total==0&&saved.game_best==0&&!strcmp(saved.name,"New"));}}
 else assert(false);
 assert(!capture_controls);return 0;
}
'''

class ConfigMessageHandlers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="passport-config-handler-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executable = Path(cls.temp.name) / "config-handler"
        source = (ROOT / "components/app/src/app.c").read_text()
        functions = "\n".join(function(source, name) for name in ("save_profile", "jsink_begin", "jsink_write", "jsink_end"))
        code = BOUNDARY + functions + '\nstatic const jpeg_rx_sink_t s_jsink={jsink_begin,jsink_write,jsink_end,NULL};\n' + function(source, "on_cfg_message") + CASES
        c = Path(cls.temp.name) / "config-handler.c"
        c.write_text(code)
        sdk = Path(os.environ.get("IDF_PATH", str(Path.home() / 'esp/esp-idf-v5.5.3'))) / "components/json/cJSON"
        services = ROOT / "components/core/services"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", str(services / "include"), "-I", str(ROOT / "components/core/ports/include"), "-I", str(sdk), str(c), *[str(services / "src" / (name + ".c")) for name in ("profile_ctl", "config_json", "score_rx", "rtttl", "jpeg_rx", "screen_capture")], str(sdk / "cJSON.c"), "-lm", "-o", str(cls.executable)], check=True, timeout=30)

    def case(self, *modes):
        for mode in modes:
            with self.subTest(mode=mode):
                subprocess.run([str(self.executable), str(mode)], check=True, timeout=5)

    def test_removed_pet_audio_and_unknown_types_are_explicitly_rejected(self): self.case(0)
    def test_sleep_and_factory_owner_gates_reject_before_any_side_effect(self): self.case(1, 2)
    def test_invalid_rtttl_never_persists_or_queues_playback(self): self.case(3)
    def test_rtttl_storage_or_missing_owner_task_preserves_previous_music(self): self.case(4, 5, 6)
    def test_valid_rtttl_persists_before_exactly_one_queue_and_duplicate_end_rejected(self): self.case(7)
    def test_jpeg_begin_busy_rejects_before_flash_sink_erase(self): self.case(8, 9, 10)
    def test_jpeg_completion_routes_slots_and_durably_marks_custom_avatar(self): self.case(11, 12)
    def test_normal_json_commits_real_serialized_profile_then_avatar_display(self): self.case(13)
    def test_json_kv_failure_reports_storage_and_never_requests_new_avatar(self): self.case(14)
    def test_mixed_settings_save_failure_preserves_runtime_and_has_no_side_effects(self): self.case(18)
    def test_mixed_settings_success_applies_all_committed_values(self): self.case(19)
    def test_time_only_update_sets_clock_without_nvs_write(self): self.case(15)
    def test_unknown_avatar_preserves_previous_source(self): self.case(16)
    def test_invalid_json_is_atomic_before_apply_or_persist(self): self.case(17)

if __name__ == "__main__": unittest.main()
