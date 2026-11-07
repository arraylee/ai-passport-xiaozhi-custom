"""Run the actual welcome gate and native prompt decoder with a screen, button and speaker double."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function

ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "xz_prompt.h"
typedef void (*xz_status_fn)(xz_phase_t,const char *);
#define pdMS_TO_TICKS(x) (x)
static xz_config_t config;
static atomic_bool welcome_visible,setup_confirmed,stop;
static unsigned now,scenario,visits,elapsed_play,writes,played,acquired,released;
static bool owned,muted,portal;
void xiaozhi_welcome_shown(void);void xiaozhi_begin_setup(void);
static void vTaskDelay(unsigned ticks){
    assert(!portal && now<600000);now+=ticks;
    if(now>=100 && scenario!=3)xiaozhi_welcome_shown();
    if(scenario==3&&now>=60)stop=true;
    if(scenario==4&&now>=20)xiaozhi_begin_setup();
    if((scenario==0||scenario==2)&&now>=1000)xiaozhi_begin_setup();
}
static void status(xz_phase_t p,const char*s){assert(p==XZ_WELCOME&&s&&!portal&&!owned);visits++;}
static bool acquire(void*u){(void)u;assert(welcome_visible&&!owned&&!portal);acquired++;if(scenario==2)return false;owned=true;return true;}
static void release(void*u){(void)u;assert(owned&&muted);owned=false;released++;}
static int rate(hal_audio_t*a,uint32_t hz,uint8_t bits,uint8_t ch){(void)a;assert(owned&&hz==24000&&bits==16&&ch==1);return 0;}
static int mute(hal_audio_t*a,bool value){(void)a;assert(owned);muted=value;return 0;}
static int close_output(hal_audio_t*a,bool on){(void)a;assert(!on&&owned&&muted);return 0;}
static FILE *captured;
static int output(hal_audio_t*a,const void*data,size_t bytes,size_t*written){
    (void)a;assert(owned&&welcome_visible&&!portal&&!muted&&bytes<=240&&bytes>0);writes++;played+=bytes/2;
    if(captured&&scenario==0)assert(fwrite(data,1,bytes,captured)==bytes);
    *written=bytes;if(scenario==1&&writes==7)xiaozhi_begin_setup();if(scenario==5&&writes==7)stop=true;return 0;
}
'''
CASES=r'''
int main(int argc,char **argv){
  hal_audio_api_t ops={.write=output,.set_sample_rate=rate,.set_out_mute=mute,.set_echo_capture=close_output};
  hal_audio_t hal={.api=&ops};config=(xz_config_t){.audio=&hal,.acquire_audio=acquire,.release_audio=release};
  if(argc==2){captured=fopen(argv[1],"wb");assert(captured);}
  for(scenario=0;scenario<6;scenario++){
    now=visits=writes=played=acquired=released=0;stop=welcome_visible=setup_confirmed=owned=muted=portal=false;
    portal=xz_wait_for_setup(&stop,status);assert(visits==1&&!owned);
    assert(portal==(scenario!=3&&scenario!=5));
    if(scenario==0)assert(writes>100&&played==EXPECTED_SAMPLES&&acquired==1&&released==1);
    if(scenario==1||scenario==5)assert(writes==7&&played==840&&released==1);
    if(scenario==2)assert(acquired==1&&!released&&!writes);
    if(scenario==3||scenario==4)assert(!acquired&&!writes);
  }
  if(captured)fclose(captured);
  puts("First use: visible portrait before local speech, complete real audio, immediate physical confirmation, cancel and speaker failure PASS");
}
'''

class WelcomeTest(unittest.TestCase):
    def test_wait_prompt_and_button(self):
        import os
        xz=ROOT/'components/platform/xiaozhi'; source=(xz/'xiaozhi.c').read_text(); asset=xz/'welcome_zh.rice'
        real='\n'.join(function(source,n) for n in ('xiaozhi_welcome_shown','xiaozhi_begin_setup','xz_wait_for_setup'))
        with tempfile.TemporaryDirectory() as directory:
            tmp=Path(directory); c=tmp/'main.c'; obj=tmp/'clip.S'; binary=tmp/'test'
            c.write_text(BOUNDARY+real+CASES.replace('EXPECTED_SAMPLES',str((xz/'welcome_zh.pcm').stat().st_size//2)))
            obj.write_text('.text\n.globl _binary_welcome_zh_rice_start\n.globl _binary_welcome_zh_rice_end\n_binary_welcome_zh_rice_start:\n.incbin "'+str(asset)+'"\n_binary_welcome_zh_rice_end:\n')
            services=ROOT/'components/core/services'
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fsanitize=undefined','-I'+str(xz),'-I'+str(xz/'include'),'-I'+str(ROOT/'components/core/ports/include'),'-I'+str(services/'include'),str(c),str(obj),str(xz/'xz_prompt.c'),str(services/'src/adpcm.c'),'-o',str(binary)],check=True)
            dump=Path(os.environ.get('PASSPORT_WELCOME_TEST_PCM',tmp/'decoded.pcm'));subprocess.run([str(binary),str(dump)],check=True,timeout=5)
            self.assertEqual(dump.read_bytes(),(xz/'welcome_zh.pcm').read_bytes())

if __name__=='__main__':unittest.main()
