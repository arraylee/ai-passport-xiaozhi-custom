"""Local fixture entry uses UI exclusion and never persists settings."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define PERIPH_DISPLAY 1
#define taskENTER_CRITICAL(x) ((void)0)
#define taskEXIT_CRITICAL(x) ((void)0)
static atomic_bool s_sleep_preparing,s_deep_sleep_entering,s_factory_command_active;
static bool s_factory_mode,s_view_nav_restore_pending,s_button_guide_active,s_button_guide_exit;
static bool capture,photo,album,voice_running,plays,profile=true,locked=true,accepted=true;
static unsigned activity,requests,opens,nav_disabled,unlocks;
static struct {int game_best;} s_profile;
static int at_reply(char*out,int n,const char*fmt,...){va_list a;va_start(a,fmt);int r=vsnprintf(out,n,fmt,a);va_end(a);return r<0?0:r<n?r:n-1;}
static bool capture_ui_blocked(void){return capture;}
static bool jpeg_view_is_active(void){return photo;}
static bool ui_album_active(void){return album;}
static bool xiaozhi_stopped(void){return !voice_running;}
static bool platform_lvgl_lock(int ms){assert(ms==100);return locked;}
static void platform_lvgl_unlock(void){unlocks++;}
static bool app_plays_active(void){return plays;}
static bool ui_profile_is_active(void){return profile;}
static bool app_plays_open(int best){assert(best==s_profile.game_best);opens++;plays=true;return true;}
static void platform_lvgl_nav_enable(bool b){assert(!b);nav_disabled++;}
static bool app_plays_voice_check(void){requests++;return plays&&accepted;}
static void note_activity(void){activity++;}
'''
CASES=r'''
int main(void){char out[160];
 s_button_guide_active=true;at_test_entry(true,out,sizeof out);
 assert(s_button_guide_exit&&!requests&&!opens&&!voice_running&&!s_factory_command_active);
 s_button_guide_active=false;at_test_entry(false,out,sizeof out);assert(strstr(out,"+OK test_home")&&!opens);
 at_test_entry(true,out,sizeof out);assert(strstr(out,"limit_ms=45000")&&requests==1&&opens==1&&nav_disabled==1&&!s_factory_command_active);
 accepted=false;at_test_entry(true,out,sizeof out);assert(strstr(out,"+ERR=busy")&&opens==1&&!s_factory_command_active);
 unsigned before=requests;voice_running=true;at_test_entry(true,out,sizeof out);assert(requests==before&&!s_factory_command_active);voice_running=false;
 bool *flags[]={&capture,&photo,&album,&s_factory_mode,&s_view_nav_restore_pending};
 for(unsigned i=0;i<sizeof flags/sizeof *flags;i++){*flags[i]=true;at_test_entry(true,out,sizeof out);assert(requests==before&&!s_factory_command_active);*flags[i]=false;}
 s_sleep_preparing=true;at_test_entry(true,out,sizeof out);assert(requests==before&&!s_factory_command_active);s_sleep_preparing=false;
 s_factory_command_active=true;at_test_entry(true,out,sizeof out);assert(requests==before&&s_factory_command_active);s_factory_command_active=false;
 locked=false;at_test_entry(true,out,sizeof out);assert(requests==before&&!s_factory_command_active);
 puts("bounded fixture entry: QR handoff, normal plays, busy/sleep/capture exclusion PASS");
}
'''
class VoiceCheckEntryTest(unittest.TestCase):
 def test_entry_ownership(self):
  source=(ROOT/'components/app/src/app.c').read_text()
  with tempfile.TemporaryDirectory() as d:
   c,e=Path(d)/'entry.c',Path(d)/'entry';c.write_text(BOUNDARY+function(source,'at_test_entry')+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
