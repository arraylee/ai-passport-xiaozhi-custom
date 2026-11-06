#include "xz_capture.h"
#include "hal/hal_audio.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t live,allocs,fail_at;
void *qa_malloc(size_t n){if(++allocs==fail_at)return NULL;void*p=malloc(n);if(p)live++;return p;}
void *qa_calloc(size_t n,size_t s){if(++allocs==fail_at)return NULL;void*p=calloc(n,s);if(p)live++;return p;}
void qa_free(void*p){if(p){assert(live);live--;free(p);}}
static int16_t far_pcm[160000],near_pcm[80000];static size_t fn,nn;
static size_t read_pcm(const char *name,int16_t *samples,size_t n){char p[1024];snprintf(p,sizeof p,"%s/%s",FIXTURES,name);FILE*f=fopen(p,"rb");assert(f);size_t got=fread(samples,sizeof *samples,n,f);fclose(f);return got;}
static int sample(const int16_t *x,size_t n,int at){return at<0?0:x[(unsigned)at%n];}
int main(void){fn=read_pcm("far-8k.pcm",far_pcm,160000);nn=read_pcm("near-8k.pcm",near_pcm,80000);
 for(unsigned rate=16000;rate<=24000;rate+=8000){xz_capture_t*c=xz_capture_create();assert(c);size_t before=allocs;
  for(int turn=0;turn<3;turn++){assert(xz_capture_begin(c,rate));assert(!xz_capture_triggered(c));int first=-1;unsigned factor=rate/8000;
   xz_bargein_metrics_t observation;
   assert(!xz_capture_observe(c,&observation));
   for(unsigned offset=0;offset<600*64*factor;offset+=HAL_AUDIO_DUPLEX_FRAMES){
    int i=(int)(offset/factor);
    if(turn==1&&i==300*64){
     assert(xz_capture_discontinuity(c));assert(!xz_capture_triggered(c));
     int16_t empty[80];assert(xz_capture_preroll(c,0,empty,80)==0);
     assert(xz_capture_gap_count(c)==1);
    }
    int16_t stereo[HAL_AUDIO_DUPLEX_FRAMES*2];
    for(unsigned k=0;k<HAL_AUDIO_DUPLEX_FRAMES;k++){
     int at=(int)((offset+k)/factor);int f=sample(far_pcm,fn,at)/2;
     int mic=sample(far_pcm,fn,at-40)/4+sample(far_pcm,fn,at-90)/8;
     if(at>=500*64)mic+=sample(near_pcm,nn,at-500*64)/2;
     stereo[2*k]=(int16_t)mic;stereo[2*k+1]=(int16_t)f;
    }
    assert(xz_capture_feed(c,stereo,HAL_AUDIO_DUPLEX_FRAMES));
    if(xz_capture_triggered(c)&&first<0)first=(int)((offset+HAL_AUDIO_DUPLEX_FRAMES)/factor); if(i<500*64)assert(!xz_capture_triggered(c));
   }
   printf("capture rate=%u turn=%d latency_ms=%d\n",rate,turn,(first-500*64)/8);assert(first>=500*64&&first<550*64);
   assert(xz_capture_observe(c,&observation));
   assert(observation.frames>=(turn==1?200:500) && observation.microphone_energy_sum>0 && observation.reference_energy_sum>0);
   assert(xz_capture_gap_count(c)==(turn==1?1:0));
   assert(observation.residual_energy_sum>0 && !observation.clipped_frames);
   assert(!xz_capture_observe(c,&observation)); /* One report per observation interval. */
   int16_t whole[2400],chunks[2400];assert(xz_capture_preroll(c,0,whole,2400)==2400);for(int j=0;j<5;j++)assert(xz_capture_preroll(c,j*480,chunks+j*480,480)==480);assert(memcmp(whole,chunks,sizeof whole)==0);assert(xz_capture_preroll(c,2400,whole,1)==0);
  }assert(allocs==before);xz_capture_destroy(c);assert(live==0);
 }
 {xz_capture_t*c=xz_capture_create();assert(c);assert(xz_capture_begin(c,24000));
  for(unsigned i=0;i<24000*2;i++){int16_t stereo[2]={i%6<3?32767:-32768,i%6<3?10000:-10000};assert(xz_capture_feed(c,stereo,1));assert(!xz_capture_triggered(c));}xz_capture_destroy(c);assert(live==0);}
 for(size_t i=1;i<=4;i++){allocs=0;fail_at=i;assert(!xz_capture_create());assert(live==0);}fail_at=0;puts("paired capture, repeated turns, pre-roll and allocation checks PASS");}
