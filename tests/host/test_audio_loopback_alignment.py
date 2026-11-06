"""Exercise the production self-test with a whole-DMA-block duplex boundary."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[2]
CODE=r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "services/audio_service.h"
static int16_t last[HAL_AUDIO_DUPLEX_FRAMES];
static bool muted;
static unsigned writes,reads;
static int error_write,error_read;
static int configure(hal_audio_t*a,uint32_t hz,uint8_t bits,uint8_t ch){(void)a;assert(hz==8000&&bits==16&&ch==1);return 0;}
static void volume(hal_audio_t*a,uint8_t v){(void)a;assert(v==60);}
static int mute(hal_audio_t*a,bool on){(void)a;muted=on;return 0;}
static int write_samples(hal_audio_t*a,const void*data,size_t n,size_t*got){(void)a;assert(n==sizeof last);writes++;if(error_write)return -1;memcpy(last,data,n);*got=n;return 0;}
static int read_samples(hal_audio_t*a,void*data,size_t n,size_t*got){(void)a;assert(n==sizeof last);reads++;if(error_read)return -1;if(muted)memset(data,0,n);else memcpy(data,last,n);*got=n;return 0;}
int main(void){
 hal_audio_api_t api={.set_sample_rate=configure,.set_volume=volume,.set_out_mute=mute,.write=write_samples,.read=read_samples};hal_audio_t hal={.api=&api};audio_service_t service={.audio=&hal,.volume=60};int peak,mag;
 assert(audio_service_loopback_test(&service,1000,300,&peak,&mag)==0);
 assert(writes==20&&reads>writes&&peak>=1990&&mag>=1990&&!muted);
 error_write=1;assert(audio_service_loopback_test(&service,1000,300,&peak,&mag)==-5);error_write=0;
 error_read=1;assert(audio_service_loopback_test(&service,1000,300,&peak,&mag)==-4);
 puts("production acoustic self-test uses complete duplex blocks;300ms tone remains coherent and I/O failures propagate PASS");
}
'''
class AudioLoopbackAlignmentTest(unittest.TestCase):
 def test_loopback_uses_coherent_whole_dma_blocks(self):
  with tempfile.TemporaryDirectory() as d:
   c,exe=Path(d)/'loopback.c',Path(d)/'loopback';c.write_text(CODE)
   subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-I',str(ROOT/'components/core/services/include'),'-I',str(ROOT/'components/core/ports/include'),str(c),str(ROOT/'components/core/services/src/audio_service.c'),'-lm','-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
