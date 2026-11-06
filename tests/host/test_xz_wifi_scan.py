"""Bound scan choices and preserve exact SSID bytes across the web form."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
CODE=r'''
#include "xz_portal.h"
#include <assert.h>
#include <string.h>
int main(void) {
  xz_wifi_option_t list[XZ_WIFI_OPTION_MAX]={0};size_t n=0;
  n=xz_wifi_option_add(list,n,(const uint8_t *)"Home",4,-60,true,1);
  n=xz_wifi_option_add(list,n,(const uint8_t *)"Guest",5,-40,false,6);
  assert(n==2&&!memcmp(list[0].ssid,"Guest",5));
  n=xz_wifi_option_add(list,n,(const uint8_t *)"Home",4,-30,true,11);
  assert(n==2&&!memcmp(list[0].ssid,"Home",4)&&list[0].rssi==-30);
  assert(xz_wifi_option_add(list,n,(const uint8_t *)"5GHz",4,-20,true,36)==n);
  assert(xz_wifi_option_add(list,n,(const uint8_t *)"",0,-20,true,1)==n);
  uint8_t bad[]={1,0,2};assert(xz_wifi_option_add(list,n,bad,3,-20,true,1)==n);
  for(unsigned i=0;i<50;i++) {uint8_t ssid[2]={'A',(uint8_t)(i+33)};n=xz_wifi_option_add(list,n,ssid,2,-(int)i,true,1);}
  assert(n==XZ_WIFI_OPTION_MAX);
  for(size_t i=1;i<n;i++)assert(list[i-1].rssi>=list[i].rssi);
  uint8_t raw[32];memset(raw,0xff,sizeof raw);char hex[65];uint8_t decoded[32];size_t len=0;
  xz_ssid_encode(raw,32,hex);assert(strlen(hex)==64);
  assert(xz_ssid_decode(hex,decoded,&len)&&len==32&&!memcmp(raw,decoded,32));
  assert(xz_ssid_decode("e5aeb6e9878c",decoded,&len)&&len==6);
  assert(!xz_ssid_decode("",decoded,&len));
  assert(!xz_ssid_decode("abc",decoded,&len));
  assert(!xz_ssid_decode("xz",decoded,&len));
  assert(!xz_ssid_decode("610062",decoded,&len));
}
'''
class WifiScanTest(unittest.TestCase):
 def test_selection_limits_dedup_sort_and_ssid_bytes(self):
  src=ROOT/'components/platform/xiaozhi'
  with tempfile.TemporaryDirectory() as folder:
   c,exe=Path(folder)/'scan.c',Path(folder)/'scan';c.write_text(CODE)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(src),str(c),str(src/'xz_portal.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
