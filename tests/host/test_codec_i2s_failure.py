"""Replay real codec format transactions with injected driver allocation errors.

Overwriting the first TX failure with RX success must fail these tests: that
exact path started NULL DMA descriptors on preview24 hardware.
"""
from pathlib import Path
import importlib.util, subprocess, tempfile, unittest
from test_ble_advertising import function
ROOT = Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('patch_codec_i2s',ROOT/'tools/patch_codec_i2s.py')
patcher=importlib.util.module_from_spec(spec);spec.loader.exec_module(patcher)
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define ESP_IDF_VERSION 50000
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_INVALID_ARG -1
#define ESP_CODEC_DEV_WRONG_STATE -2
#define ESP_CODEC_DEV_NOT_SUPPORT -3
#define ESP_CODEC_DEV_TYPE_IN 1
#define ESP_CODEC_DEV_TYPE_OUT 2
#define ESP_CODEC_DEV_TYPE_IN_OUT 3
#define ESP_CODEC_DEV_MAKE_CHANNEL_MASK(x) (1U<<(x))
#define ESP_LOGI(...)
#define TAG "test"
typedef int esp_codec_dev_type_t;
typedef struct {uint32_t sample_rate;uint8_t bits_per_sample,channel;uint16_t channel_mask;} esp_codec_dev_sample_info_t;
typedef void *i2s_chan_handle_t;
typedef struct {int unused;} i2s_chan_info_t;
typedef struct {bool is_open,out_enable,in_enable,in_disable_pending,out_disable_pending;void *in_handle,*out_handle;int clk_src;esp_codec_dev_sample_info_t in_fs,out_fs,fs;} i2s_data_t;
typedef i2s_data_t audio_codec_data_if_t;
static int fault, calls, enables, locks, fail_enable, disables;
static i2s_data_t peer;
static i2s_data_t *get_paired(i2s_data_t*d,bool playback){(void)d;(void)playback;return &peer;}
static uint8_t get_bits(i2s_data_t*d,bool p){(void)d;(void)p;return 16;}
static int i2s_channel_get_info(void*c,i2s_chan_info_t*i){(void)c;(void)i;return 0;}
static int set_drv_fs(void*c,bool playback,uint8_t bits,int clk,esp_codec_dev_sample_info_t*f){(void)c;(void)playback;(void)bits;(void)clk;(void)f;return ++calls==fault?-77:0;}
static int _i2s_drv_enable(i2s_data_t*d,bool p,bool e){(void)d;(void)p;if(e){enables++;if(fail_enable==(p?1:2))return -88;}else disables++;return 0;}
static bool _i2s_valid_fmt(esp_codec_dev_sample_info_t*f){(void)f;return true;}
static void _i2s_lock(i2s_data_t*d,const char*f){(void)d;(void)f;locks++;}
static void _i2s_unlock(i2s_data_t*d){(void)d;locks--;}
static int check_fs_compatible(i2s_data_t*d,bool p,esp_codec_dev_sample_info_t*f){(void)d;(void)p;(void)f;return 0;}
'''
CASES=r'''
int main(void){
 i2s_data_t d={.is_open=true,.out_handle=(void*)1,.in_handle=(void*)2};
 esp_codec_dev_sample_info_t f={.sample_rate=24000,.bits_per_sample=16,.channel=2,.channel_mask=3};
 calls=0;fault=1;assert(_i2s_data_set_fmt(&d,3,&f)==-77);assert(calls==1&&locks==0);
 calls=0;fault=2;assert(_i2s_data_set_fmt(&d,3,&f)==-77);assert(calls==2&&locks==0);
 calls=0;fault=0;assert(_i2s_data_set_fmt(&d,3,&f)==0);assert(calls==2&&locks==0);
 peer.out_handle=(void*)3;peer.out_enable=false;
 calls=enables=0;fault=2;assert(set_fs(&d,false,false)==-77);assert(enables==0);
 calls=enables=0;fault=0;assert(set_fs(&d,false,false)==0);assert(enables==1);
 fail_enable=1;enables=disables=0;assert(_i2s_data_enable(&d,3,true)==-88);assert(enables==1&&!d.in_enable&&!d.out_enable);
 fail_enable=2;enables=disables=0;assert(_i2s_data_enable(&d,3,true)==-88);assert(enables==2&&disables==1&&!d.in_enable&&!d.out_enable);
 fail_enable=0;enables=disables=0;assert(_i2s_data_enable(&d,3,true)==0);assert(d.in_enable&&d.out_enable);
 assert(_i2s_data_enable(&d,3,false)==0);assert(!d.in_enable&&!d.out_enable&&disables==2);
 return 0;
}
'''
class CodecI2sFailure(unittest.TestCase):
 def test_tx_failure_is_preserved_and_failed_peer_is_not_started(self):
  source=patcher.patch((ROOT/'managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c').read_text())
  code=BOUNDARY+'\n'+function(source,'set_fs')+'\n'+function(source,'_i2s_data_set_fmt')+'\n'+function(source,'_i2s_data_enable')+CASES
  with tempfile.TemporaryDirectory() as d:
   c,e=Path(d)/'test.c',Path(d)/'test';c.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True,timeout=5)
 def test_unreviewed_dependency_change_is_rejected(self):
  with self.assertRaises(ValueError):patcher.patch('new upstream')
if __name__=='__main__':unittest.main()
