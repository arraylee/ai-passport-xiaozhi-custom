/* Run the shipped VAD, farewell audio and intent parser against bounded hardware doubles. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xz_quiet.h"
#include "xz_prompt.h"
#include "xz_protocol.h"
#include "services/adpcm.h"

static atomic_bool cancel, end_now;
static bool owned, muted, fail_acquire, fail_write, fail_rate, cancel_on_write;
static unsigned acquisitions, releases, closes, writes;
static int16_t output_pcm[32000];
static size_t output_count;

static bool acquire(void *user) {
  (void)user;assert(!owned);++acquisitions;
  if(fail_acquire)return false;
  owned=true;muted=true;return true;
}
static void release(void *user) { (void)user;assert(owned&&muted);owned=false;++releases; }
static int rate(hal_audio_t *a,uint32_t hz,uint8_t bits,uint8_t channels) {
  (void)a;assert(owned&&hz==16000&&bits==16&&channels==1);return fail_rate?-1:0;
}
static int mute(hal_audio_t *a,bool v) { (void)a;assert(owned);muted=v;return 0; }
static int close_codec(hal_audio_t *a,bool on) { (void)a;assert(owned&&muted&&!on);++closes;return 0; }
static int output(hal_audio_t *a,const void *pcm,size_t bytes,size_t *written) {
  (void)a;assert(owned&&!muted&&bytes&&bytes<=HAL_AUDIO_DUPLEX_FRAMES*2&&bytes%2==0);
  assert(output_count+bytes/2<=sizeof output_pcm/sizeof output_pcm[0]);
  memcpy(output_pcm+output_count,pcm,bytes);output_count+=bytes/2;*written=fail_write?0:bytes;
  if(cancel_on_write)atomic_store(&end_now,true);
  ++writes;return 0;
}
static size_t load(const char *path,uint8_t *bytes,size_t capacity) {
  FILE *f=fopen(path,"rb");assert(f);size_t n=fread(bytes,1,capacity,f);
  assert(n&&feof(f));fclose(f);return n;
}
static void test_sound(const char *dump_path) {
  static const hal_audio_api_t api={.write=output,.set_sample_rate=rate,.set_out_mute=mute,.set_echo_capture=close_codec};
  hal_audio_t hal={.api=&api};xz_config_t config={.audio=&hal,.acquire_audio=acquire,.release_audio=release};
  uint8_t data[16000];size_t size=load(FAREWELL_FILE,data,sizeof data);
  size_t n=xz_play_prompt(&config,&cancel,&end_now,data,size);
  assert(n==size*2&&output_count==n&&n>4000&&releases==1&&closes==1&&writes>4&&!owned);
  int16_t ref[32000];adpcm_state_t decoder;adpcm_state_reset(&decoder);
  assert((size_t)adpcm_decode(&decoder,data,(int)size,ref)==n);
  assert(!memcmp(ref,output_pcm,n*sizeof *ref));
  int peak=0;for(size_t i=0;i<n;i++)if(abs(output_pcm[i])>peak)peak=abs(output_pcm[i]);
  assert(peak>5000&&peak<32767);for(size_t i=n-1200;i<n;i++)assert(abs(output_pcm[i])<32);
  if(dump_path){FILE*f=fopen(dump_path,"wb");assert(f);assert(fwrite(output_pcm,sizeof *output_pcm,n,f)==n);fclose(f);}
  printf("Goodbye uses %zu bytes in Flash, %zu real PCM samples at 16 kHz, peak %d.\n",size,n,peak);
  fail_acquire=true;assert(!xz_play_prompt(&config,&cancel,&end_now,data,size)&&!owned&&releases==1);fail_acquire=false;
  fail_rate=true;assert(!xz_play_prompt(&config,&cancel,&end_now,data,size)&&!owned&&releases==2);fail_rate=false;
  fail_write=true;assert(!xz_play_prompt(&config,&cancel,&end_now,data,size)&&!owned&&releases==3);fail_write=false;
  cancel_on_write=true;unsigned before=writes;assert(!xz_play_prompt(&config,&cancel,&end_now,data,size)&&writes==before+1&&releases==4&&!owned);
  unsigned tries=acquisitions;assert(!xz_play_prompt(&config,&cancel,&end_now,data,size)&&acquisitions==tries);
}
static void test_clock(void) {
  xz_voice_pcm_t buf={0};xz_quiet_t timer={0};xz_quiet_begin_listening(&buf);
  for(int i=0;i<499;i++)assert(!xz_quiet_feed(&timer,&buf));
  assert(timer.quiet_samples==479040);assert(xz_quiet_feed(&timer,&buf));
  timer.quiet_samples=0;xz_quiet_begin_listening(&buf);
  for(int i=0;i<334;i++)assert(!xz_quiet_feed(&timer,&buf));
  xz_quiet_begin_listening(&buf); // Cloud renewal must preserve the elapsed silence.
  for(int i=334;i<499;i++)assert(!xz_quiet_feed(&timer,&buf));
  assert(xz_quiet_feed(&timer,&buf));
  timer.quiet_samples=0;xz_quiet_begin_listening(&buf);
  for(int i=0;i<475;i++)assert(!xz_quiet_feed(&timer,&buf));
  // Use real decoded Chinese speech from the distributed asset at 28.5 s.
  size_t total=output_count;assert(total>8000);bool detected=false;
  for(size_t at=0;at+960<=total;at+=960){
    memcpy(buf.listen.samples,output_pcm+at,sizeof buf.listen.samples);
    assert(!xz_quiet_feed(&timer,&buf));if(timer.quiet_samples==0)detected=true;
  }
  assert(detected&&timer.quiet_samples<16000);memset(buf.listen.samples,0,sizeof buf.listen.samples);
  unsigned frames=0;while(!xz_quiet_feed(&timer,&buf)){assert(++frames<525);}
  assert(frames>=480);assert(sizeof buf==2880);
  printf("Real VAD: silence at 30,000 ms, speech at 28.5 s resets clock; cloud renewal preserves clock; PCM buffer %zu bytes.\n",sizeof buf);
}
static void test_intent(void) {
  const char *yes[]={"拜拜","再见！","好了，小智，拜拜啦","我得去开会了，咱们下次再聊。","那我们今天就先聊到这里吧。","我先走啦，下次见！","谢谢，我们回头聊","再见小智","我不想聊了","bye-bye!"};
  const char *no[]={"","请告诉我再见怎么说","我不想说再见","别跟我说拜拜","如果我说拜拜，你会怎么样","他说，拜拜。","我想听这个故事，再见是什么意思？","再见面的时候","你能教我说拜拜吗","不要结束聊天","我正在听课","你可以说‘拜拜’吗"};
  for(size_t i=0;i<sizeof yes/sizeof *yes;i++){if(!xz_is_farewell(yes[i]))fprintf(stderr,"missed intent %zu\n",i);assert(xz_is_farewell(yes[i]));}
  for(size_t i=0;i<sizeof no/sizeof *no;i++)assert(!xz_is_farewell(no[i]));
  puts("Direct farewells, indirect conversation closings, questions, negation and mentions PASS.");
}
int main(int argc,char **argv){test_sound(argc==2?argv[1]:NULL);test_clock();test_intent();return 0;}
