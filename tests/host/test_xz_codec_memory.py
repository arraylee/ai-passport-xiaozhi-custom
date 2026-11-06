"""Production codec configuration/error metrics against deterministic boundaries."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ESP_AUDIO_ERR_OK 0
#define MALLOC_CAP_8BIT 1
#define ESP_OPUS_ENC_FRAME_DURATION_60_MS 5
#define ESP_OPUS_DEC_FRAME_DURATION_60_MS 5
typedef struct { int sample_rate,channel,bits_per_sample,bitrate,frame_duration,complexity,application_mode; bool enable_dtx; } esp_opus_enc_config_t;
typedef struct { int sample_rate,channel,frame_duration; bool self_delimited; } esp_opus_dec_cfg_t;
#define ESP_OPUS_ENC_CONFIG_DEFAULT() { .application_mode=0 }
static int requested_error;
static unsigned metrics;
static const char *TAG="fixture";
static struct { int stage,phase; } working={3,0};
static unsigned esp_get_free_heap_size(void){return 17288;}
static unsigned heap_caps_get_largest_free_block(int cap){assert(cap==1);return 7680;}
static unsigned uxTaskGetStackHighWaterMark(void *task){assert(!task);return 19968;}
static int opus_encoder_get_size(int channels){assert(channels==1);return 24524;}
static int opus_decoder_get_size(int channels){assert(channels==1);return 17784;}
static char last_log[320];
static void record(const char *tag,const char *format,...) {
 assert(tag==TAG);metrics++;va_list ap;va_start(ap,format);
 vsnprintf(last_log,sizeof last_log,format,ap);va_end(ap);
}
#define ESP_LOGI(...) record(__VA_ARGS__)
#define ESP_LOGW(...) record(__VA_ARGS__)
static int esp_opus_enc_open(void *ptr,int n,void **out) {
 assert(n==sizeof(esp_opus_enc_config_t));esp_opus_enc_config_t *c=ptr;
 assert(c->sample_rate==16000&&c->channel==1&&c->bits_per_sample==16);
 assert(c->bitrate==16000&&c->frame_duration==5&&c->complexity==0&&c->enable_dtx);
 *out=requested_error?NULL:(void *)1;return requested_error;
}
static int esp_opus_dec_open(void *ptr,int n,void **out) {
 assert(n==sizeof(esp_opus_dec_cfg_t));esp_opus_dec_cfg_t *c=ptr;
 assert((c->sample_rate==16000||c->sample_rate==24000)&&c->channel==1&&c->frame_duration==5&&!c->self_delimited);
 *out=requested_error?NULL:(void *)1;return requested_error;
}
'''
CASES = r'''
int main(void) {
 void *codec=NULL;
 const int errors[]={0,-1,-2,-5};
 for(unsigned i=0;i<sizeof errors/sizeof errors[0];i++){
  requested_error=errors[i];metrics=0;last_log[0]=0;
  assert(encoder_open(&codec)==(requested_error==0));
  assert(metrics>=1 && "encoder return code/heap/largest/stack must be observable without payloads");
  char rc[24];snprintf(rc,sizeof rc,"rc=%d",requested_error);
  assert(strstr(last_log,rc)&&strstr(last_log,"heap=17288")&&strstr(last_log,"largest=7680")&&strstr(last_log,"stack_free=19968"));
  assert(strstr(last_log,"need=24524")&&strstr(last_log,"stage=3"));
  metrics=0;assert(decoder_open(24000,&codec)==(requested_error==0));
  assert(metrics>=1&&strstr(last_log,rc)&&strstr(last_log,"need=17784"));
 }
 puts("codec metrics preserve config, all exact return codes and numeric-only allocation state PASS");
}
'''

PROBE_BOUNDARY = r'''
#define XZ_PACKET_MAX 1275
#define XZ_ERROR 9
typedef struct { uint8_t *buffer; unsigned len; } esp_audio_enc_in_frame_t;
typedef struct { uint8_t *buffer; unsigned len,encoded_bytes; } esp_audio_enc_out_frame_t;
typedef struct { uint8_t *buffer; unsigned len; } esp_audio_dec_in_raw_t;
typedef struct { uint8_t *buffer; unsigned len,decoded_size; } esp_audio_dec_out_frame_t;
typedef struct { int unused; } esp_audio_dec_info_t;
static unsigned probe_scenario,enc_frames,dec_frames,enc_closes,dec_closes,probe_allocations,live_buffers;
static int dec_rate;
static void status(int phase,const char *reason){assert(reason&&reason[0]);working.phase=phase;}
static void *probe_malloc(size_t n){
 ++probe_allocations;
 if((probe_scenario==1 && probe_allocations==1)||(probe_scenario==2 && probe_allocations==2))return NULL;
 void *p=malloc(n);assert(p);live_buffers++;return p;
}
static void *probe_calloc(size_t n,size_t size){void *p=probe_malloc(n*size);if(p)memset(p,0,n*size);return p;}
static void probe_free(void *p){if(p){assert(live_buffers);live_buffers--;free(p);}}
static int esp_opus_enc_process(void *codec,esp_audio_enc_in_frame_t *in,esp_audio_enc_out_frame_t *out){
 assert(codec==(void *)1&&in->len==1920&&out->len==1275);
 unsigned nonzero=0;for(unsigned i=0;i<in->len;i++)nonzero+=in->buffer[i]!=0;
 assert(nonzero>100);enc_frames++;
 if(probe_scenario==4)return -1;
 memcpy(out->buffer,"TEST",4);out->encoded_bytes=4;return 0;
}
static int esp_opus_dec_decode(void *codec,esp_audio_dec_in_raw_t *raw,esp_audio_dec_out_frame_t *frame,esp_audio_dec_info_t *info){
 assert(codec==(void *)1&&raw->len==4&&!memcmp(raw->buffer,"TEST",4)&&frame->len==2880);(void)info;dec_frames++;
 if((probe_scenario==6&&dec_rate==16000)||(probe_scenario==8&&dec_rate==24000))return -1;
 frame->decoded_size=dec_rate*60/1000*2-(probe_scenario==9?2:0);return 0;
}
static void esp_opus_enc_close(void *p){assert(p==(void *)1);enc_closes++;}
static void esp_opus_dec_close(void *p){assert(p==(void *)1);dec_closes++;}
#define malloc probe_malloc
#define calloc probe_calloc
#define free probe_free
'''
PROBE_CASES = r'''
int main(void){
 for(probe_scenario=0;probe_scenario<11;probe_scenario++){
  requested_error=probe_scenario==3?-2:0;working.phase=0;
  enc_frames=dec_frames=enc_closes=dec_closes=probe_allocations=live_buffers=0;
  assert(codec_probe()==(probe_scenario==0));
  assert(!live_buffers);
  if(probe_scenario==0)assert(enc_frames==1&&dec_frames==2&&enc_closes==1&&dec_closes==2);
  else assert(working.phase==XZ_ERROR);
  if(probe_scenario>=4)assert(enc_closes==1);
 }
 puts("local nonzero codec frame is measured at both decoder rates, never sent or played, and cleans every failure PASS");
}
'''


class CodecMemoryTest(unittest.TestCase):
    def test_local_probe_exercises_one_nonzero_encode_and_both_decode_rates_with_cleanup(self):
        from test_xz_channel_memory import function
        source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        funcs=function(source,'codec_metric')+function(source,'encoder_open')+function(source,'decoder_open')
        probe=function(source,'codec_probe') if 'static bool codec_probe(' in source else 'static bool codec_probe(void){return true;}'
        boundary=BOUNDARY.replace('static int requested_error;', 'static int requested_error;\nstatic unsigned probe_scenario;\nstatic int dec_rate;')
        boundary=boundary.replace(' *out=requested_error?NULL:(void *)1;return requested_error;\n}\n', ' *out=requested_error?NULL:(void *)1;return requested_error;\n}\n',1)
        boundary=boundary.replace('return 19968;', 'return probe_scenario==10?1024:19968;')
        marker=' *out=requested_error?NULL:(void *)1;return requested_error;'
        last=boundary.rindex(marker)
        boundary=boundary[:last]+' dec_rate=c->sample_rate;\n int error=((probe_scenario==5&&dec_rate==16000)||(probe_scenario==7&&dec_rate==24000))?-2:requested_error;\n *out=error?NULL:(void *)1;return error;'+boundary[last+len(marker):]
        stubs=PROBE_BOUNDARY.replace('static unsigned probe_scenario,enc_frames','static unsigned enc_frames').replace('static int dec_rate;','')
        code=boundary+funcs+stubs+'\n#define XZ_CODEC_STACK_RESERVE 2048u\n'+probe+PROBE_CASES
        with tempfile.TemporaryDirectory(prefix='passport-codec-probe-') as folder:
            src,exe=Path(folder)/'probe.c',Path(folder)/'probe'
            src.write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-fsanitize=undefined',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

    def test_packet_payload_is_safe_for_in_place_opus_ctr_send_and_receive(self):
        from test_xz_channel_memory import function
        source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
static atomic_bool cancel, requested_end;
static unsigned delay_ms;
static void vTaskDelay(unsigned milliseconds){assert(milliseconds==10);delay_ms+=milliseconds;}
#define pdMS_TO_TICKS(value) (value)
static unsigned numeric_logs;
static void numeric_log(const char *fmt,...){(void)fmt;++numeric_logs;}
#define ESP_LOGW(tag,...) numeric_log(__VA_ARGS__)
#define ESP_LOGI(tag,...) numeric_log(__VA_ARGS__)
#define XZ_PACKET_MAX 1275
typedef struct {int aes,udp;uint8_t nonce[16];uint32_t tx_sequence,tx_retries,tx_dropped;} channel_t;
static uint8_t sent[1291];static size_t sent_size;static bool send_error;
static uint64_t ms(void){return 1234;}
static int mbedtls_aes_crypt_ctr(int *ctx,size_t n,size_t *offset,uint8_t *nonce,uint8_t *stream,const uint8_t *in,uint8_t *out){
 (void)ctx;(void)nonce;(void)stream;assert(*offset==0);
 for(size_t i=0;i<n;i++)out[i]=in[i]^0x5a;return 0;
}
static int fixture_send(int fd,const void *data,size_t n,int flags){if(send_error){errno=ENOMEM;return -1;}assert(fd==7&&!flags&&n<=sizeof sent);memcpy(sent,data,n);sent_size=n;return n;}
#define send fixture_send
'''+function(source,'audio_crypt')+function(source,'send_audio')+r'''
int main(void){channel_t c={.udp=7};uint8_t packet[1291];c.nonce[0]=1;
 for(unsigned n=1;n<=1275;n+=1274){
  for(unsigned i=0;i<n;i++)packet[i+16]=(uint8_t)i;
  assert(send_audio(&c,packet+16,n,packet));assert(sent_size==n+16);
  assert(packet[0]==1&&(((unsigned)packet[2]<<8)|packet[3])==n);
  for(unsigned i=0;i<n;i++)assert(sent[i+16]==((uint8_t)i^0x5a));
  assert(audio_crypt(&c,packet+16,n,packet,packet+16));
  for(unsigned i=0;i<n;i++)assert(packet[i+16]==(uint8_t)i);
 }
 assert(!send_audio(&c,packet+16,0,packet));assert(!send_audio(&c,packet+16,1276,packet));
 send_error=true;assert(send_audio(&c,packet+16,1,packet));assert(numeric_logs==3&&c.tx_dropped==1&&delay_ms==20);
}
'''
        with tempfile.TemporaryDirectory(prefix='passport-codec-alias-') as folder:
            src,exe=Path(folder)/'alias.c',Path(folder)/'alias'
            src.write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)

    def test_actual_codec_errors_and_numeric_allocation_state_are_retained(self):
        from test_xz_channel_memory import function
        source=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text()
        helper=function(source,'codec_metric') if 'static void codec_metric(' in source else ''
        code=BOUNDARY+helper+function(source,'encoder_open')+function(source,'decoder_open')+CASES
        with tempfile.TemporaryDirectory(prefix='passport-codec-metrics-') as folder:
            src,exe=Path(folder)/'metric.c',Path(folder)/'metric'
            src.write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable',
                '-fsanitize=undefined',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=5)


if __name__=='__main__':
    unittest.main()
