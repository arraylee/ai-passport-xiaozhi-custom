/* Synthetic fixed TTS only; saturation is in the speaker path, not the ADC. */
#include "xz_bargein.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
static size_t live,allocs;
void *qa_malloc(size_t n){void*p=malloc(n);allocs++;if(p)live++;return p;}
void *qa_calloc(size_t n,size_t s){void*p=calloc(n,s);allocs++;if(p)live++;return p;}
void qa_free(void*p){if(p){assert(live);live--;free(p);}}
static int16_t far_pcm[160000],near_pcm[80000];
static size_t far_n,near_n;
static size_t read_pcm(const char *name,int16_t *samples,size_t n){char p[1024];snprintf(p,sizeof p,"%s/%s",FIXTURES,name);FILE*f=fopen(p,"rb");assert(f);size_t got=fread(samples,sizeof *samples,n,f);assert(feof(f));fclose(f);return got;}
static uint32_t rng=24;
static int rnd(void){rng=rng*1664525u+1013904223u;return(int)(rng>>16)-32768;}
static int16_t clamp(int x){return x>32767?32767:x<-32768?-32768:(int16_t)x;}
static int sample(const int16_t *x,size_t n,int at){return at<0?0:x[(unsigned)at%n];}
static int limit;
static int acoustic_case(unsigned delay,int near_start,int near_gain,int echo_gain,int noise,int echo_step){
 xz_bargein_t*b=xz_bargein_create(delay);assert(b);size_t alloc_start=allocs;
 int16_t ref[64],mic[64];int first=-1;const int frames=1900;
 for(int frame=0;frame<frames;frame++){
  for(int i=0;i<64;i++){
   int at=frame*64+i;
   ref[i]=(int16_t)(sample(far_pcm,far_n,at)/2);
   int gain=echo_gain+(echo_step&&frame>850?echo_step:0);
   int echo=(sample(far_pcm,far_n,at-(int)delay-40)*gain/100 +sample(far_pcm,far_n,at-(int)delay-90)*gain/200)/2;
   if (echo>limit) echo=limit; if(echo<-limit) echo=-limit;
   int near=near_start>=0&&frame>=near_start?sample(near_pcm,near_n,at-near_start*64)*near_gain/100:0;
   mic[i]=clamp(echo+near+rnd()*noise/32768);
  }
  assert(xz_bargein_feed(b,ref,mic));
  if(xz_bargein_triggered(b)&&first<0)first=frame;
 }
 assert(allocs==alloc_start);xz_bargein_destroy(b);assert(live==0);
 printf("speech delay=%u near=%d gain=%d echo=%d noise=%d step=%d trigger=%d latency_ms=%d\n",delay,near_start,near_gain,echo_gain,noise,echo_step,first,first>=0&&near_start>=0?(first-near_start)*8:-1);
 return first;
}
int main(void){
 far_n=read_pcm("far-8k.pcm",far_pcm,sizeof far_pcm/2);near_n=read_pcm("near-8k.pcm",near_pcm,sizeof near_pcm/2);
 for (limit=1000;limit<=8000;limit*=2) {
 assert(acoustic_case(0,-1,0,100,25,0)==-1);
 int onset=acoustic_case(0,500,50,100,25,0);
 assert(onset>=500 && onset<=560);
 }
 puts("nonlinear echo rejection and independent speech PASS");return 0;
}
