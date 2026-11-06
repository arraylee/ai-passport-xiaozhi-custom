"""Execute deployed profile migration and actual device-setting callbacks."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
R=Path(__file__).resolve().parents[2]
class DeviceSettings(unittest.TestCase):
 def test_migration_preview_save_and_failure(self):
  boundary=r'''
#define PERIPH_DISPLAY 1
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include "services/profile_ctl.h"
static profile_data_t s_profile,disk;
static void *s_disp=(void*)1;
static unsigned writes,effective=100,activity;
static bool fail;
static void hal_display_set_backlight(void*d,uint8_t value){assert(d==s_disp);effective=value;}
static bool save_profile(void){writes++;if(fail)return false;disk=s_profile;return true;}
static void note_activity(void){activity++;}
'''
  cases=r'''
int main(void){
 profile_ctl_init(&s_profile);assert(s_profile.sleep_min==5&&s_profile.brightness==100);
 strcpy(s_profile.name,"MIGRATION");s_profile.volume=100;s_profile.game_best=234;
 unsigned choices[]={0,3,10,17,1440};
 for(unsigned i=0;i<5;i++){
  s_profile.sleep_min=choices[i];
  uint8_t old[1+offsetof(profile_data_t,brightness)];old[0]=16;memcpy(old+1,&s_profile,sizeof old-1);
  profile_data_t next;assert(profile_deserialize(&next,old,sizeof old)==0);
  assert(next.sleep_min==(choices[i]==10?5:(int)choices[i]));
  assert(next.brightness==100&&next.volume==100&&next.game_best==234&&!strcmp(next.name,"MIGRATION"));
 }
 profile_ctl_init(&s_profile);
 assert(!plays_preview_brightness(0,0)&&!plays_preview_brightness(0,9)&&!plays_preview_brightness(0,101));
 assert(effective==100&&!writes);
 assert(plays_preview_brightness(0,40)&&effective==40&&s_profile.brightness==100&&!writes);
 assert(plays_preview_brightness(0,plays_get_brightness(0))&&effective==100&&!writes);
 assert(plays_set_brightness(0,40)&&effective==40&&disk.brightness==40&&writes==1);
 assert(plays_set_brightness(0,40)&&writes==1);
 assert(plays_preview_brightness(0,70)&&effective==70);
 fail=true;assert(!plays_set_brightness(0,70)&&effective==40&&s_profile.brightness==40&&disk.brightness==40);
 fail=false;assert(plays_set_sleep_minutes(0,0)&&plays_get_sleep_minutes(0)==0&&activity==1);
 unsigned count=writes;assert(plays_set_sleep_minutes(0,0)&&writes==count&&activity==2);
 fail=true;assert(!plays_set_sleep_minutes(0,30)&&plays_get_sleep_minutes(0)==0&&activity==2);
 fail=false;assert(!plays_set_sleep_minutes(0,1441)&&plays_set_sleep_minutes(0,30));
 s_profile.brightness=10;uint8_t blob[PROFILE_BLOB_SIZE];assert(profile_serialize(&s_profile,blob,sizeof blob)>0);
 profile_data_t next;assert(profile_deserialize(&next,blob,sizeof blob)==0&&next.brightness==10&&next.sleep_min==30);
 s_profile.brightness=0;profile_serialize(&s_profile,blob,sizeof blob);assert(profile_deserialize(&next,blob,sizeof blob)==0&&next.brightness==100);
 return 0;
}
'''
  app=(R/'components/app/src/app.c').read_text();names=['plays_get_brightness','plays_preview_brightness','plays_set_brightness','plays_get_sleep_minutes','plays_set_sleep_minutes']
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.c').write_text(boundary+'\n'.join(function(app,n) for n in names)+cases)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(R/'components/core/services/include'),str(p/'test.c'),str(R/'components/core/services/src/profile_ctl.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
