"""Replay real conversation loop with real cJSON and deterministic audio/network boundaries."""
from pathlib import Path
import os,subprocess,tempfile,unittest
from test_xz_channel_memory import function,block
ROOT=Path(__file__).resolve().parents[2]
BOUNDARY=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <errno.h>
#define ESP_LOGW(...) ((void)0)
#include "cJSON.h"
#define HAL_AUDIO_DUPLEX_FRAMES 120
#define XZ_PACKET_MAX 1275
#define XZ_CODEC_STACK_RESERVE 2048
#define ESP_AUDIO_ERR_OK 0
#define MSG_DONTWAIT 1
#define pdMS_TO_TICKS(x) (x)
enum {XZ_READY,XZ_LISTEN,XZ_THINK,XZ_SPEAK,XZ_ERROR,XZ_OFF};
static struct {int phase;} working;
static atomic_bool running,cancel,requested_talk,requested_interrupt,requested_end;
enum {SESSION_CLOSED,SESSION_PARK,SESSION_RENEW,SESSION_QUIET_BYE,SESSION_INTENT_BYE,SESSION_AUDIO_LOST};
typedef struct {unsigned quiet_samples;} xz_quiet_t;
typedef union { int16_t playback[1440]; struct {int16_t samples[960];} listen; } xz_voice_pcm_t;
static int last_end;
static void xz_quiet_begin_listening(xz_voice_pcm_t*b){(void)b;}
static bool xz_quiet_feed(xz_quiet_t*q,xz_voice_pcm_t*b){(void)b;assert(working.phase==XZ_LISTEN);q->quiet_samples+=960;return q->quiet_samples>=480000;}
static unsigned echo_reads,echo_begins,idle_yields;
static atomic_uint avatar;
static uint64_t now;
static unsigned starts,stops,aborts,captured,played,stt_seen,enc_closes,dec_closes,owned,scenario;
static bool enc_live,dec_live,queue_ready,audio_delivered;
static unsigned message_cursor, udp_polls;
static bool duplicate_delivered,echo_live,fake_trigger,text_message;
static unsigned preroll_frames, pending_reference, written_bytes;
static char last_reason[160];
static char sent[512];
static unsigned summaries,reported_received,reported_valid,reported_written;
static unsigned esp_get_free_heap_size(void){return 30000;}
static unsigned heap_caps_get_largest_free_block(int caps){(void)caps;return 12000;}
#define MALLOC_CAP_8BIT 1
static void record(const char *format,...){
 if(strcmp(format,"tts received=%u valid=%u written=%u"))return;
 va_list ap;va_start(ap,format);reported_received=va_arg(ap,unsigned);reported_valid=va_arg(ap,unsigned);reported_written=va_arg(ap,unsigned);va_end(ap);summaries++;
}
static uint64_t ms(void){return now;}
static int64_t esp_timer_get_time(void){return (int64_t)now*1000;}
static void status(int phase,const char *s){working.phase=phase;snprintf(last_reason,sizeof last_reason,"%s",s);if(!strcmp(s,"recognized"))stt_seen++;}
static void persist_avatar(unsigned a){(void)a;}
static void codec_metric(unsigned a,unsigned b,int rc){(void)a;(void)b;(void)rc;}
static void audio_heap_metric(unsigned stage){(void)stage;}
static unsigned uxTaskGetStackHighWaterMark(void *p){(void)p;return 4508;}
static void vTaskDelay(unsigned n){now+=n;if(n==1)idle_yields++;}
static bool acquire(void *u){(void)u;assert(!owned);if(scenario==1)return false;owned=1;if(scenario==4)cancel=true;return true;}
static void release(void *u){(void)u;assert(owned);owned=0;pending_reference=0;}
static struct {void *audio,*user;bool(*acquire_audio)(void*);void(*release_audio)(void*);} config={(void*)1,NULL,acquire,release};
static bool encoder_open(void **p){assert(!enc_live&&!dec_live);if(scenario==2)return false;enc_live=true;*p=(void*)1;return true;}
static void esp_opus_enc_close(void *p){assert(p==(void*)1&&enc_live&&!dec_live);enc_live=false;enc_closes++;}
static bool decoder_open(int rate,void **p){assert(rate==24000&&!dec_live&&!enc_live);dec_live=true;*p=(void*)2;return true;}
static void esp_opus_dec_close(void *p){assert(p==(void*)2&&dec_live&&!enc_live);dec_live=false;dec_closes++;}
static int hal_audio_set_sample_rate(void *a,int hz,int b,int ch){assert(a&&owned&&hz==24000&&b==16&&ch==1);return 0;}
static int hal_audio_set_out_mute(void *a,bool mute){assert(a);(void)mute;return 0;}
static int hal_audio_read(void *a,void *pcm,size_t n,size_t *got){assert(a&&owned&&enc_live&&!dec_live&&n==1920);memset(pcm,0,n);*got=n;now+=60;captured++;if(starts==4)cancel=true;return 0;}
static int hal_audio_write(void *a,void *pcm,size_t n,size_t *w){(void)pcm;assert(a&&owned&&dec_live&&!enc_live&&n==HAL_AUDIO_DUPLEX_FRAMES*2);*w=0;if(scenario==17&&pending_reference+n/2>840)return -7;*w=n;written_bytes+=n;pending_reference+=n/2;if(scenario==20&&!aborts)requested_interrupt=true;if(scenario==22&&written_bytes==240)now+=36000;return 0;}
typedef struct {uint8_t *buffer;unsigned len;} esp_audio_enc_in_frame_t;
typedef struct {uint8_t *buffer;unsigned len,encoded_bytes;} esp_audio_enc_out_frame_t;
typedef struct {uint8_t *buffer;unsigned len;} esp_audio_dec_in_raw_t;
typedef struct {uint8_t *buffer;unsigned len,decoded_size;} esp_audio_dec_out_frame_t;
typedef struct {int unused;} esp_audio_dec_info_t;
static int esp_opus_enc_process(void *p,esp_audio_enc_in_frame_t *i,esp_audio_enc_out_frame_t *o){assert(p&&enc_live&&!dec_live&&i->len==1920);if(fake_trigger&&preroll_frames<5){int16_t *v=(int16_t*)i->buffer;for(unsigned k=0;k<960;k++)assert(v[k]==(int16_t)(preroll_frames*480+k/2));preroll_frames++;if(scenario==16&&preroll_frames==3)return -1;}o->encoded_bytes=2;return 0;}
static int esp_opus_dec_decode(void *p,esp_audio_dec_in_raw_t *i,esp_audio_dec_out_frame_t *o,esp_audio_dec_info_t *x){(void)i;(void)x;assert(p&&dec_live&&!enc_live);o->decoded_size=scenario==18?2878:2880;played++;return 0;}
typedef struct {int udp,rate;uint32_t rx_sequence;char session[128];atomic_bool failed;atomic_int error;} channel_t;
typedef struct {char data[];} invalid_dummy_t;
typedef struct {unsigned total;char data[];} channel_message_t;
enum {CHANNEL_CONTROL_PARSE};
static const char *channel_error_text(channel_t *c){(void)c;return "control error";}
static bool send_text(channel_t *c,const char *s){(void)c;snprintf(sent,sizeof sent,"%s",s);return scenario!=3 && !(scenario==15&&strstr(s,"\"abort\""));}
static bool send_audio(channel_t *c,const uint8_t *o,size_t n,uint8_t *p){(void)c;(void)o;(void)n;(void)p;if(fake_trigger&&preroll_frames<6){if(preroll_frames==5)fake_trigger=false;return true;}if(scenario==21)return true;if(captured%2==0){queue_ready=true;message_cursor=udp_polls=0;audio_delivered=duplicate_delivered=text_message=false;}return true;}
static channel_message_t *channel_take(channel_t *c,unsigned timeout){(void)c;(void)timeout;
 static const char *messages[]={"{\"type\":\"stt\",\"text\":\"recognized\"}","{\"type\":\"tts\",\"state\":\"start\"}","{\"type\":\"tts\",\"state\":\"stop\"}"};
 if(!queue_ready&&scenario==11&&played&&!duplicate_delivered){
  duplicate_delivered=true;const char *duplicate="{\"type\":\"tts\",\"state\":\"start\"}";
  size_t n=strlen(duplicate);channel_message_t *m=malloc(sizeof *m+n+1);assert(m);memcpy(m->data,duplicate,n+1);return m;
 }
 if(!queue_ready)return NULL;const char *msg;
 if(scenario>=24&&message_cursor==2&&!text_message){text_message=true;msg="{\"type\":\"tts\",\"state\":\"sentence_start\",\"text\":\"A real spoken answer\"}";}
 else {msg=messages[message_cursor++];if(scenario==25&&message_cursor==3)msg="{\"type\":\"notice\"}";}if(scenario==23&&message_cursor==1)msg="{\"type\":\"stt\",\"text\":\"我先走了，下次聊。\"}";size_t n=strlen(msg);channel_message_t *m=malloc(sizeof *m+n+1);assert(m);memcpy(m->data,msg,n+1);if(scenario==5)strcpy(m->data,"{");if(message_cursor==3)queue_ready=false;return m;
}
static int recv(int fd,void *p,size_t n,int flags){errno=EAGAIN;if(scenario==27&&working.phase==XZ_SPEAK){errno=ECONNRESET;return -1;}(void)fd;(void)p;(void)n;if(scenario==7&&played)cancel=true;if(flags==MSG_DONTWAIT)return -1;assert(working.phase!=XZ_LISTEN);now+=250;udp_polls++;if(scenario==8||scenario==24||scenario==25||(scenario==26&&udp_polls<=5)||(scenario==10&&udp_polls==1))return -1;if(scenario==7&&played)cancel=true;if(working.phase==XZ_SPEAK&&!audio_delivered){audio_delivered=true;static uint32_t wire_sequence;uint8_t *wire=p;memset(wire,0,18);wire[0]=scenario==9?2:1;wire[3]=2;uint32_t seq=++wire_sequence;wire[12]=seq>>24;wire[13]=seq>>16;wire[14]=seq>>8;wire[15]=seq;return 18;}return -1;}
typedef void xz_capture_t;
static bool xz_capture_begin(xz_capture_t *p,unsigned rate){assert(p&&rate==24000&&!echo_live);echo_begins++;fake_trigger=false;return true;}
static bool xz_capture_triggered(xz_capture_t *p){return p&&fake_trigger;}
static size_t xz_capture_preroll(xz_capture_t *p,size_t offset,int16_t*out,size_t cap){assert(p&&offset<2400&&cap==480);for(size_t i=0;i<cap;i++)out[i]=(int16_t)(offset+i);return cap;}
static void xz_capture_destroy(xz_capture_t *p){(void)p;}
static int hal_audio_set_echo_capture(void*a,bool enable){assert(a);if(enable&&scenario==13)return -1;echo_live=enable;return 0;}
static struct {unsigned write_peak_us,writes;bool pending;} audio_timing;
static void timing_peak(uint32_t *p,int64_t start){(void)p;(void)start;}
static void audio_timing_report(xz_capture_t*p,uint32_t echo,uint32_t decode){assert(!echo_live);(void)p;(void)echo;(void)decode;audio_timing.pending=false;}
static bool capture_reply(xz_capture_t*p,unsigned rate,size_t frames,uint32_t*peak){assert(p&&echo_live&&owned&&dec_live&&!enc_live&&rate==24000);echo_reads++;*peak=900;if(pending_reference>=frames)pending_reference-=frames;else pending_reference=0;now+=frames*1000/rate;if((scenario==12||scenario==15||scenario==16)&&played==1&&aborts==0)fake_trigger=true;return scenario!=14;}
static bool audio_crypt(channel_t *c,void *in,size_t n,void *nonce,void *out){(void)c;(void)in;(void)n;(void)nonce;(void)out;return true;}
'''
CASES=r'''
int main(void){
 channel_t channel={.rate=24000},*c=&channel;
 assert(command(c,"listen","start"));assert(strstr(sent,"\"mode\":\"auto\""));
 running=true;requested_talk=false;xiaozhi_interrupt();assert(requested_interrupt&&!cancel);requested_interrupt=false;xiaozhi_talk();assert(requested_end&&!cancel&&!requested_talk);requested_end=false;
 starts=stops=aborts=0;run_loop();
 assert(starts==4&&stops==0&&aborts==0&&played==3&&stt_seen==3&&written_bytes==8640);
 assert(enc_closes==4&&dec_closes==3&&!enc_live&&!dec_live&&!owned);
 assert(summaries==3&&reported_received==1&&reported_valid==1&&reported_written==1);
 for(scenario=1;scenario<=7;scenario++){
  if(scenario==6)continue;
  cancel=false;requested_talk=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=false;
  starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=0;now=0;
  run_loop();assert(!owned&&!enc_live&&!dec_live&&!stops&&!aborts);
  if(scenario==4||scenario==7)assert(cancel);else assert(working.phase==XZ_ERROR);
 }
 for(scenario=8;scenario<=11;scenario++){
  cancel=false;requested_talk=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=false;
  starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=0;summaries=reported_received=reported_valid=reported_written=0;now=0;last_reason[0]=0;
  run_loop();assert(!owned&&!enc_live&&!dec_live&&!stops&&!aborts);
  if(scenario==8){
   assert(working.phase==XZ_LISTEN&&starts==4&&played==0&&cancel);
   assert(summaries==3&&reported_received==0&&reported_valid==0&&reported_written==0);
  }else if(scenario==9){
   assert(working.phase==XZ_ERROR&&starts==1&&played==0&&strstr(last_reason,"未收到语音回复"));
   assert(summaries==1&&reported_received==(scenario==8?0U:1U)&&reported_valid==0&&reported_written==0);
  }else assert(starts==4&&played==3&&cancel&&summaries==3&&reported_written==1);
 }
 for(scenario=12;scenario<=16;scenario++){
  cancel=false;requested_talk=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=echo_live=fake_trigger=false;
  starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=preroll_frames=0;now=0;
  run_loop();assert(!owned&&!enc_live&&!dec_live&&!echo_live&&!stops);
  if(scenario==12)assert(aborts==1&&starts==4&&played==3&&preroll_frames==5&&cancel);
  else {assert(working.phase==XZ_ERROR);if(scenario==16)assert(aborts==1&&preroll_frames==3);}
 }
 scenario=17;cancel=false;requested_talk=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=echo_live=fake_trigger=false;
 starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=preroll_frames=pending_reference=written_bytes=0;now=0;
 run_loop();assert(cancel&&starts==4&&played==3&&written_bytes==8640&&!owned&&!enc_live&&!dec_live);
 scenario=18;cancel=false;requested_talk=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=echo_live=fake_trigger=false;
 starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=preroll_frames=pending_reference=written_bytes=0;now=0;
 run_loop();assert(working.phase==XZ_ERROR&&played==1&&written_bytes==0&&!owned&&!enc_live&&!dec_live&&!echo_live);
 for(scenario=19;scenario<=20;scenario++){
  cancel=requested_talk=requested_interrupt=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=echo_live=fake_trigger=false;
  starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=preroll_frames=pending_reference=written_bytes=echo_reads=echo_begins=idle_yields=0;now=0;
  run_loop();assert(cancel&&starts==4&&played==3&&!owned&&!enc_live&&!dec_live&&!echo_live);
  assert(!echo_reads&&!echo_begins&&!preroll_frames&&idle_yields>0);
  if(scenario==19)assert(aborts==0&&written_bytes==8640);
  else assert(aborts==1&&written_bytes==6000);
 }
 for(scenario=21;scenario<=23;scenario++){
  cancel=requested_talk=requested_interrupt=requested_end=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=echo_live=fake_trigger=false;
  starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=preroll_frames=pending_reference=written_bytes=0;now=0;
  run_loop();assert(!owned&&!enc_live&&!dec_live&&!echo_live);
  if(scenario==21)assert(captured==500&&now==30000&&last_end==SESSION_QUIET_BYE&&played==0);
  if(scenario==22)assert(last_end==SESSION_CLOSED&&played==3&&starts==4&&now>36000);
  if(scenario==23)assert(last_end==SESSION_INTENT_BYE&&captured==2&&played==0&&starts==1);
 }
 for(scenario=24;scenario<=27;scenario++){
  cancel=requested_talk=requested_interrupt=requested_end=false;owned=0;enc_live=dec_live=queue_ready=audio_delivered=duplicate_delivered=echo_live=fake_trigger=text_message=false;
  starts=stops=aborts=captured=played=stt_seen=enc_closes=dec_closes=message_cursor=udp_polls=preroll_frames=pending_reference=written_bytes=0;now=0;
  run_loop();assert(!owned&&!enc_live&&!dec_live&&!echo_live);
  if(scenario==26)assert(cancel&&played==3&&starts==4);
  else {assert(last_end==SESSION_AUDIO_LOST&&played==0&&starts==1);if(scenario==24)assert(now>=1500&&now<2000);if(scenario==25)assert(now>=10000&&now<10500);}
 }
 puts("Voiced text with zero audio renews transport, genuinely empty replies relisten, late UDP grace and terminal recv errors PASS");
 puts("natural abort/pre-roll/relisten and failure cleanup; automatic turns, codec exclusion, cleanup, empty reply relisten, invalid UDP rejection, late audio and duplicate TTS start PASS");
}
'''
class AutoDialogTest(unittest.TestCase):
 def test_auto_rounds_and_confirm_end_use_production_state_machine(self):
  s=(ROOT/'components/platform/xiaozhi/xiaozhi.c').read_text();run=function(s,'run_session')
  loop=block(run,run.index('  while (!atomic_load(&cancel) && !atomic_load(&c->failed))'))
  cleanup=run[run.index('  if (audio_owned)',run.index('\nfinish:')):run.index('  if (c) {',run.index('\nfinish:'))]
  # Keep real command serialization and instrument only successful send boundaries.
  command=function(s,'command').replace('bool ok = s && send_text(c, s);','bool ok = s && send_text(c, s);\n  if(ok&&!strcmp(type,"listen")&&state&&!strcmp(state,"start")){starts++;assert(!dec_live);}\n  if(ok&&!strcmp(type,"listen")&&state&&!strcmp(state,"stop"))stops++;\n  if(ok&&!strcmp(type,"abort"))aborts++;')
  protocol=(ROOT/'components/platform/xiaozhi/xz_protocol.c').read_text()
  code=BOUNDARY.replace('typedef struct {char data[];} invalid_dummy_t;\n','')+'\n#include "xz_protocol.h"\n'+command+function(s,'xiaozhi_talk')+function(s,'xiaozhi_interrupt')+function(s,'discard_reply_audio')+function(s,'send_preroll')+function(s,'reply_audio_missing')
  setup='''static void run_loop(void){xz_quiet_t clock={0},*quiet=&clock;int outcome=SESSION_CLOSED;channel_t channel={.rate=24000},*c=&channel;void *encoder=NULL,*decoder=NULL;bool audio_owned=false,echo_owned=false,ignore_aborted_tts=false,pending_preroll=false;xz_capture_t*capture=scenario>=19?NULL:(void*)1;uint32_t capture_peak_us=0,decode_peak_us=0;uint8_t packet[XZ_PACKET_MAX+16],*opus=packet+16;xz_voice_pcm_t storage={0},*audio_buffer=&storage;int16_t *pcm=storage.playback;unsigned saved_avatar=0;uint64_t last_action=0,listen_at=0,last_audio=0;unsigned missing_count=1,*missing_replies=&missing_count;bool tts_text=false,tts_stop=false,encoder_measured=false,decoder_measured=false;uint32_t tts_received=0,tts_valid=0,tts_written=0;working.phase=XZ_READY;'''
  prefix=run[run.index('  last_action = ms(); // Provisioning'):run.index('  while (!atomic_load(&cancel) && !atomic_load(&c->failed))')]
  code=code.replace('#define pdMS_TO_TICKS(x) (x)', '#define pdMS_TO_TICKS(x) (x)\n#define ESP_LOGI(tag,...) record(__VA_ARGS__)')
  code+=setup+prefix+loop+cleanup+'last_end=outcome;}\n'+CASES
  idf=Path(os.environ.get('IDF_PATH',str(Path.home() / 'esp/esp-idf-v5.5.3')));json=idf/'components/json/cJSON'
  with tempfile.TemporaryDirectory() as d:
   c,exe=Path(d)/'auto.c',Path(d)/'auto';c.write_text(code)
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-function','-Werror','-fsanitize=undefined','-I',str(json),'-I',str(ROOT/'components/platform/xiaozhi'),str(c),str(ROOT/'components/platform/xiaozhi/xz_protocol.c'),str(json/'cJSON.c'),'-o',str(exe)],check=True)
   subprocess.run([str(exe)],check=True,timeout=5)

if __name__=='__main__':unittest.main()
