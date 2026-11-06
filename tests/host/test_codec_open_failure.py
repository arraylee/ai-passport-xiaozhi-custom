"""The codec must not enable DMA after set_fmt fails, and retry must reopen."""
from pathlib import Path
import subprocess,tempfile,unittest
from test_codec_i2s_failure import ROOT,patcher
from test_ble_advertising import function
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#define ESP_CODEC_DEV_INVALID_ARG -1
#define ESP_CODEC_DEV_NOT_SUPPORT -2
#define ESP_CODEC_DEV_DRV_ERR -3
#define ESP_CODEC_DEV_OK 0
#define ESP_CODEC_DEV_TYPE_IN 1
#define ESP_CODEC_DEV_TYPE_OUT 2
#define VOL_TRANSITION_TIME 50
#define ESP_LOGI(...)
#define ESP_LOGE(...)
typedef void *esp_codec_dev_handle_t;
typedef struct {int unused;} esp_codec_dev_sample_info_t;
typedef struct audio_codec_if_t {
 int (*set_fs)(const struct audio_codec_if_t*,esp_codec_dev_sample_info_t*);
 int (*enable)(const struct audio_codec_if_t*,bool);
 int (*set_vol)(void);
} audio_codec_if_t;
typedef struct audio_codec_data_if_t {
 int (*set_fmt)(const struct audio_codec_data_if_t*,int,esp_codec_dev_sample_info_t*);
 int (*enable)(const struct audio_codec_data_if_t*,int,bool);
} audio_codec_data_if_t;
typedef struct audio_codec_vol_if_t {int(*open)(const struct audio_codec_vol_if_t*,esp_codec_dev_sample_info_t*,int);} audio_codec_vol_if_t;
typedef struct {bool input_opened,output_opened,sw_vol_alloced;int dev_caps;const audio_codec_if_t*codec_if;const audio_codec_data_if_t*data_if;const audio_codec_vol_if_t*sw_vol;} codec_dev_t;
static int fault,fmt_calls,starts,disables,codec_started;
static bool _verify_drv_ready(codec_dev_t*d,bool out){(void)d;(void)out;return true;}
static const audio_codec_vol_if_t*audio_codec_new_sw_vol(void){return NULL;}
static void _update_codec_setting(codec_dev_t*d){(void)d;}
static int fmt(const audio_codec_data_if_t*d,int caps,esp_codec_dev_sample_info_t*f){(void)d;(void)caps;(void)f;fmt_calls++;return fault==1?-77:0;}
static int en(const audio_codec_data_if_t*d,int caps,bool on){(void)d;(void)caps;if(on){starts++;return fault==2?-78:0;}disables++;return 0;}
static int codec_fmt(const audio_codec_if_t*c,esp_codec_dev_sample_info_t*f){(void)c;(void)f;return fault==3?-79:0;}
static int codec_enable(const audio_codec_if_t*c,bool on){(void)c;codec_started=on;return on&&fault==4?-80:0;}
static int volume(void){return 0;}
'''
CASES=r'''
int main(void){
 const audio_codec_data_if_t data={fmt,en};const audio_codec_if_t codec={codec_fmt,codec_enable,volume};
 esp_codec_dev_sample_info_t f={0};
 for(fault=1;fault<=4;fault++){
  codec_dev_t d={.dev_caps=3,.data_if=&data,.codec_if=&codec};fmt_calls=starts=disables=codec_started=0;
  assert(esp_codec_dev_open(&d,&f)!=0);
  assert(!d.input_opened&&!d.output_opened&&!codec_started);
  assert(starts==(fault==1?0:1));
  int previous=fault;fault=0;
  assert(esp_codec_dev_open(&d,&f)==0&&fmt_calls==2&&d.input_opened&&d.output_opened&&codec_started);
  fault=previous;
 }
 return 0;
}
'''
class CodecOpenFailure(unittest.TestCase):
 def test_failed_format_and_enable_roll_back_before_retry(self):
  source=patcher.patch_device((ROOT/'managed_components/espressif__esp_codec_dev/esp_codec_dev.c').read_text())
  with tempfile.TemporaryDirectory() as d:
   c,e=Path(d)/'test.c',Path(d)/'test';c.write_text(BOUNDARY+function(source,'esp_codec_dev_open')+CASES)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),'-o',str(e)],check=True)
   subprocess.run([str(e)],check=True,timeout=5)
if __name__=='__main__':unittest.main()
