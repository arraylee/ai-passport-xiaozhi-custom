"""Run the pinned IDF read/write code with a nearly full DMA completion queue.

The old128/192 combination silently skips partial blocks, without a queue
overflow. Whole120-frame transfers must preserve every stereo sample.
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
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "hal/hal_audio.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG -1
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_TIMEOUT -3
#define I2S_DIR_TX 1
#define I2S_DIR_RX 2
#define I2S_CHAN_STATE_RUNNING 1
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(x) (x)
#define I2S_NULL_POINTER_CHECK(tag,p) assert(p)
#define ESP_RETURN_ON_FALSE(c,e,...) do {if(!(c))return (e);} while(0)
typedef struct {uint32_t data[32][128];unsigned at;} queue_t;
typedef struct {
 int dir,state,binary;
 struct {size_t rw_pos,buf_size;unsigned desc_num;void *curr_ptr;} dma;
 queue_t *msg_queue;
} *i2s_chan_handle_t;
static int xSemaphoreTake(int s,unsigned t){(void)s;(void)t;return pdTRUE;}
static void xSemaphoreGive(int s){(void)s;}
// Replenish completions between calls; never invoke overflow or drop a queue
// entry. This isolates the driver's partial-block guard from real overruns.
static unsigned uxQueueSpacesAvailable(queue_t*q){(void)q;return 1;}
static int xQueueReceive(queue_t*q,void **out,unsigned t){
 (void)t;assert(q->at<32);*out=q->data[q->at++];return pdTRUE;
}
'''
HAL_BOUNDARY = r'''
typedef struct {void *dev;bool echo_capture;uint8_t cur_bits,cur_ch;} es_ctx_t;
static int esp_codec_dev_write(void*d,void*p,size_t n){size_t got=0;int r=i2s_channel_write(d,p,n,&got,100);return r?r:(got==n?0:-1);}
static int esp_codec_dev_read(void*d,void*p,size_t n){size_t got=0;int r=i2s_channel_read(d,p,n,&got,100);return r?r:(got==n?0:-1);}
'''
CASES = r'''
static unsigned receive(unsigned block,unsigned slice) {
 queue_t q={0};
 for(unsigned b=0;b<32;b++)for(unsigned i=0;i<block;i++)q.data[b][i]=b*block+i;
 __typeof__(*((i2s_chan_handle_t)0)) channel={.dir=I2S_DIR_RX,.state=I2S_CHAN_STATE_RUNNING,.dma={.buf_size=block*4,.desc_num=7},.msg_queue=&q};
 uint32_t out[384];unsigned gaps=0;
 for(unsigned call=0;call<2;call++){
  size_t got=0;assert(i2s_channel_read(&channel,out+call*slice,slice*4,&got,100)==0&&got==slice*4);
 }
 for(unsigned i=0;i<slice*2;i++)gaps+=(out[i]!=i);
 return gaps;
}
static unsigned transmit(unsigned block,unsigned slice) {
 queue_t q={0};for(unsigned b=0;b<32;b++)for(unsigned i=0;i<128;i++)q.data[b][i]=UINT32_MAX;
 __typeof__(*((i2s_chan_handle_t)0)) channel={.dir=I2S_DIR_TX,.state=I2S_CHAN_STATE_RUNNING,.dma={.buf_size=block*4,.desc_num=7},.msg_queue=&q};
 uint32_t input[384];for(unsigned i=0;i<384;i++)input[i]=i;
 for(unsigned call=0;call<2;call++){
  size_t got=0;assert(i2s_channel_write(&channel,input+call*slice,slice*4,&got,100)==0&&got==slice*4);
 }
 unsigned gaps=0;for(unsigned i=0;i<slice*2;i++)gaps+=(q.data[i/block][i%block]!=i);
 return gaps;
}
static void hal_stream(void) {
 for(unsigned samples=960;samples<=1440;samples+=480){
  queue_t q={0};
  __typeof__(*((i2s_chan_handle_t)0)) channel={.dir=I2S_DIR_TX,.state=I2S_CHAN_STATE_RUNNING,.dma={.buf_size=HAL_AUDIO_DUPLEX_FRAMES*4,.desc_num=7},.msg_queue=&q};
  es_ctx_t ctx={.dev=&channel,.cur_bits=16,.cur_ch=1};hal_audio_t audio={.impl=&ctx};
  int16_t pcm[1440];for(unsigned i=0;i<samples;i++)pcm[i]=(int16_t)i;
  size_t got=0;assert(es_write(&audio,pcm,samples*2,&got)==0&&got==samples*2);
  for(unsigned i=0;i<samples;i++)assert(q.data[i/HAL_AUDIO_DUPLEX_FRAMES][i%HAL_AUDIO_DUPLEX_FRAMES]==(i|(i<<16)));
  q.at=0;channel.dir=I2S_DIR_RX;channel.dma.curr_ptr=NULL;channel.dma.rw_pos=0;
  assert(es_read(&audio,pcm,samples*2,&got)==0&&got==samples*2);
  for(unsigned i=0;i<samples;i++)assert(pcm[i]==(int16_t)i);
 }
}
int main(void){
 unsigned old_rx=receive(128,192),old_tx=transmit(128,192);
 assert(old_rx>0&&old_tx>0);
 assert(960%HAL_AUDIO_DUPLEX_FRAMES==0&&1440%HAL_AUDIO_DUPLEX_FRAMES==0);
 assert(receive(HAL_AUDIO_DUPLEX_FRAMES,HAL_AUDIO_DUPLEX_FRAMES)==0);
 assert(transmit(HAL_AUDIO_DUPLEX_FRAMES,HAL_AUDIO_DUPLEX_FRAMES)==0);
 hal_stream();
 printf("old partial blocks: RX %u mismatches TX %u; aligned duplex blocks: 0\n",old_rx,old_tx);
}
'''

class I2SDmaAlignmentTest(unittest.TestCase):
    def test_pinned_driver_preserves_complete_duplex_blocks(self):
        idf = Path(os.environ.get('IDF_PATH', str(Path.home()/'esp/esp-idf-v5.5.3')))
        source = (idf/'components/esp_driver_i2s/i2s_common.c').read_text()
        hal = (ROOT/'components/platform/platform_esp32/src/audio_es8311.c').read_text()
        code = BOUNDARY + '\n'.join(function(source,n) for n in ['i2s_channel_write','i2s_channel_read'])
        code += HAL_BOUNDARY + '\n'.join(function(hal,n) for n in ['es_write','es_read']) + CASES
        with tempfile.TemporaryDirectory() as folder:
            c, exe = Path(folder)/'dma.c', Path(folder)/'dma'
            c.write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',
                            '-I',str(ROOT/'components/core/ports/include'),str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

if __name__ == '__main__':
    unittest.main()
