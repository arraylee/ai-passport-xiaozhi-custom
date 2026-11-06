"""Link the real ES8311 TX-complete meter and XiaoZhi output query into native LVGL tests."""
from pathlib import Path
import sys

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from test_ble_advertising import function

hal, worker, out = map(Path, sys.argv[1:])
source=hal.read_text().replace('bool IRAM_ATTR ', 'bool ')
head='''#include <stdatomic.h>
#include "xiaozhi.h"
typedef void *i2s_chan_handle_t;
typedef struct {void *dma_buf;size_t size;} i2s_event_data_t;
typedef struct {atomic_uint meter_enabled, audible_tail;} es_ctx_t;
'''
body='\n'.join(function(source,name) for name in ('es_tx_sent','es_output_active','es_meter_enable'))
tail='''static es_ctx_t speaker;
static const hal_audio_api_t ops={.output_active=es_output_active};
static hal_audio_t dev={.api=&ops,.impl=&speaker};
static xz_config_t config={.audio=&dev};
void native_tx_reset(void){es_meter_enable(&speaker,false);es_meter_enable(&speaker,true);}
void native_tx_complete(int16_t *stereo,size_t samples){i2s_event_data_t e={stereo,samples*sizeof(*stereo)};es_tx_sent(NULL,&e,&speaker);}
'''
out.write_text(head+body+'\n'+tail+function(worker.read_text(),'xiaozhi_output_active'))
