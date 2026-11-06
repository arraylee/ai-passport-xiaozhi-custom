"""Exercise ES8311 temporary stereo echo mode, write expansion and rollback."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_ble_advertising import function
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef void* esp_codec_dev_handle_t;
typedef struct {esp_codec_dev_handle_t dev;void*ctrl_if;unsigned cur_hz;uint8_t cur_bits,cur_ch,requested_volume;bool opened,echo_capture,out_muted;atomic_uint meter_enabled,audible_tail;volatile uint32_t rx_overflows;uint32_t echo_overflows_seen;} es_ctx_t;
#define IRAM_ATTR
#define HAL_AUDIO_ECHO_GAP 1
#define HAL_AUDIO_DUPLEX_FRAMES 120
typedef void *i2s_chan_handle_t;
typedef struct {void *dma_buf;size_t size;} i2s_event_data_t;
typedef struct {es_ctx_t*impl;} hal_audio_t;
static int error_write,error_read,error_reg,error_rate;static unsigned writes,reads,channels;static uint8_t reg44;
static int16_t output[2000];static size_t output_n;
static int applied_volume,volume_error;
static int esp_codec_dev_set_out_vol(void*d,int v){assert(d);applied_volume=v;return volume_error?-11:0;}
static int esp_codec_dev_write(void*d,void*p,size_t n){assert(d);writes++;if(error_write&&writes==(unsigned)error_write)return -7;memcpy(output+output_n,p,n);output_n+=n/2;return 0;}
static int esp_codec_dev_read(void*d,void*p,size_t n){assert(d);reads++;if(error_read)return -8;for(size_t i=0;i<n/2;i++)((int16_t*)p)[i]=(i%2)?-700:(int16_t)(100+i/2);return 0;}
static int es_ctrl_write(void*c,uint8_t r,uint8_t v){(void)c;assert(r==0x44);reg44=v;return error_reg?-9:0;}
static int es_rate(hal_audio_t*a,uint32_t hz,uint8_t bits,uint8_t ch){assert(hz==16000||hz==24000);assert(bits==16);channels=ch;a->impl->opened=!error_rate;return error_rate?-10:0;}
static int esp_codec_dev_set_out_mute(void*d,bool mute){assert(d&&mute);return 0;}
static void es_close(es_ctx_t*c){c->opened=false;}
'''
CASES=r'''
int main(void){
 es_ctx_t c={.dev=(void*)1,.cur_hz=24000,.opened=true};hal_audio_t a={&c};
 int16_t bus[240]={0};i2s_event_data_t eof={.dma_buf=bus,.size=sizeof bus};
 es_meter_enable(&c,true);assert(!es_output_active(&a));
 for(unsigned packet=0;packet<14;packet++){es_tx_sent(0,&eof,&c);assert(!es_output_active(&a));}
 bus[0]=30000;es_tx_sent(0,&eof,&c);assert(!es_output_active(&a)); // One transient is not speech.
 bus[16]=-1500;es_tx_sent(0,&eof,&c);assert(es_output_active(&a));
 memset(bus,0,sizeof bus);for(unsigned i=0;i<7;i++)es_tx_sent(0,&eof,&c);assert(!es_output_active(&a));
 bus[0]=bus[16]=900;es_tx_sent(0,&eof,&c);assert(es_output_active(&a));
 es_meter_enable(&c,false);es_tx_sent(0,&eof,&c);assert(!es_output_active(&a));
 es_meter_enable(&c,true);assert(!es_output_active(&a));
 puts("Tx-complete PCM: silent queue and one spike keep mouth closed, actual voiced DMA opens it, silence/mute closes it");
 int16_t mono[317];for(int i=0;i<317;i++)mono[i]=(int16_t)(i-150);size_t n=99;
 es_volume(&a,95);assert(applied_volume==95);
 assert(es_echo_capture(&a,true)==0&&c.echo_capture&&channels==2&&reg44==0x58);
 assert(applied_volume==95);
 es_volume(&a,60);assert(applied_volume==60);
 es_volume(&a,100);assert(applied_volume==95);
 assert(es_write(&a,mono,sizeof mono,&n)==0&&n==sizeof mono&&output_n==634);
 for(int i=0;i<317;i++)assert(output[2*i]==mono[i]&&output[2*i+1]==mono[i]);
 int16_t stereo[128];assert(es_read_echo(&a,stereo,64,&n)==0&&n==64);
 assert(!es_rx_overflow(NULL,NULL,&c));assert(!es_rx_overflow(NULL,NULL,&c));
 assert(es_read_echo(&a,stereo,64,&n)==HAL_AUDIO_ECHO_GAP&&n==64);
 assert(c.echo_overflows_seen==2);
 assert(es_read_echo(&a,stereo,64,&n)==0&&n==64);
 error_read=1;assert(es_read_echo(&a,stereo,64,&n)!=0&&n==0);error_read=0;
 writes=0;error_write=2;assert(es_write(&a,mono,sizeof mono,&n)!=0&&n==HAL_AUDIO_DUPLEX_FRAMES*2);error_write=0;
 assert(es_echo_capture(&a,false)==0&&!c.echo_capture&&!c.opened&&reg44==0x08);
 assert(applied_volume==100);
 assert(es_read_echo(&a,stereo,64,&n)!=0&&n==0);
 c.opened=true;volume_error=1;assert(es_echo_capture(&a,true)!=0&&!c.echo_capture&&!c.opened);volume_error=0;
 es_volume(&a,95);assert(applied_volume==95);
 c.opened=true;error_rate=1;assert(es_echo_capture(&a,true)!=0&&!c.echo_capture&&!c.opened&&reg44==0x08);error_rate=0;
 c.opened=true;error_reg=1;assert(es_echo_capture(&a,true)!=0&&!c.echo_capture&&!c.opened);error_reg=0;
 c.cur_ch=1;c.cur_bits=16;c.opened=true;output_n=writes=0;
 assert(es_write(&a,mono,sizeof mono,&n)==0&&n==sizeof mono&&output_n==634);
 for(int i=0;i<317;i++)assert(output[2*i]==mono[i]&&output[2*i+1]==mono[i]);
 int16_t mic[3];assert(es_read(&a,mic,sizeof mic,&n)==0&&n==sizeof mic);
 assert(mic[0]==100&&mic[1]==101&&mic[2]==102);
 c.dev=NULL;assert(es_write(&a,mono,sizeof mono,&n)!=0&&n==0);assert(es_echo_capture(&a,true)!=0);
 return 0;
}
'''
class AudioEchoTest(unittest.TestCase):
 def test_failed_dma_reconfiguration_rebuilds_before_enabling(self):
  source=(ROOT/'components/platform/platform_esp32/src/audio_es8311.c').read_text()
  boundary=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stddef.h>
typedef void*esp_codec_dev_handle_t;
typedef struct {void*dev,*tx,*rx;bool opened,i2s_needs_enable,out_muted;uint8_t requested_volume;atomic_uint meter_enabled,audible_tail;unsigned cur_hz;uint8_t cur_bits,cur_ch;} es_ctx_t;
typedef struct {es_ctx_t*impl;} hal_audio_t;
typedef struct {unsigned bits_per_sample,channel,channel_mask,sample_rate,mclk_multiple;} esp_codec_dev_sample_info_t;
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(x) (1U<<(x))
#define ESP_LOGD(...)
#define ESP_LOGE(...)
static bool dma_valid=true;static int open_error;
static void es_close(es_ctx_t*c){c->opened=false;c->i2s_needs_enable=true;}
static int i2s_channel_enable(void*c){(void)c;assert(dma_valid);return 0;}
static int esp_codec_dev_open(void*d,esp_codec_dev_sample_info_t*f){(void)d;assert(f->channel==2&&f->channel_mask==3);dma_valid=!open_error;return open_error?-1:0;}
static int esp_codec_dev_set_in_gain(void*d,float gain){(void)d;(void)gain;return 0;}
static int esp_codec_dev_set_out_mute(void*d,bool mute){(void)d;(void)mute;return 0;}
'''
  cases=r'''
int main(void){es_ctx_t c={.dev=(void*)1,.tx=(void*)2,.rx=(void*)3};hal_audio_t a={&c};
assert(es_rate(&a,16000,16,1)==0&&c.cur_ch==1);
open_error=1;assert(es_rate(&a,24000,16,2)!=0&&!c.opened&&!dma_valid);
open_error=0;assert(es_rate(&a,24000,16,2)==0&&c.opened&&dma_valid);return 0;}
'''
  with tempfile.TemporaryDirectory() as d:
   c,e=Path(d)/'test.c',Path(d)/'test';c.write_text(boundary+function(source,'es_meter_enable')+function(source,'es_rate')+cases)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Werror','-fsanitize=undefined',str(c),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True,timeout=5)
 def test_stereo_echo_and_rollback(self):
  s=(ROOT/'components/platform/platform_esp32/src/audio_es8311.c').read_text().replace('bool IRAM_ATTR ', 'bool ')
  names=['es_meter_enable','es_tx_sent','es_output_active','es_rx_overflow','es_write','es_read','es_apply_volume','es_volume','es_echo_capture','es_read_echo']
  f='\n'.join(function(s,n) for n in names)
  with tempfile.TemporaryDirectory() as d:
   c,e=Path(d)/'test.c',Path(d)/'test';c.write_text(BOUNDARY+f+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
