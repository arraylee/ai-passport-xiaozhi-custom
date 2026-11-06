#include "xz_bargein.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static size_t live,allocs,fail_at;
void *qa_malloc(size_t n){if(++allocs==fail_at)return NULL;void*p=malloc(n);if(p)live++;return p;}
void *qa_calloc(size_t n,size_t s){if(++allocs==fail_at)return NULL;void*p=calloc(n,s);if(p)live++;return p;}
void qa_free(void*p){if(p){assert(live);live--;free(p);}}
static uint32_t rng=24;
static int random_word(void){rng=rng*1664525u+1013904223u;return(int)(rng>>16)-32768;}
static void acoustic_case(unsigned delay,bool clipped,bool near_only){
 xz_bargein_t*b=xz_bargein_create(delay);assert(b);size_t setup_allocs=allocs;
 int16_t ref[64],mic[64],hist[4096]={0};int cursor=0,smooth=0,first=-1;const int start=near_only?60:500;
 for(int frame=0;frame<start+60;frame++){
  for(int i=0;i<64;i++){
   smooth=(smooth*3+random_word()/3)/4;ref[i]=near_only?0:(int16_t)smooth;hist[cursor&4095]=ref[i];
   int echo=hist[(cursor-(int)delay-40)&4095]/2+hist[(cursor-(int)delay-90)&4095]/4;
   mic[i]=(int16_t)(echo+(frame>=start?random_word()/5:0));
   if(clipped)mic[i]=i%2?32767:-32768;
   cursor++;
  }
  assert(xz_bargein_feed(b,ref,mic));
  if(xz_bargein_triggered(b)&&first<0)first=frame;
  if((frame<start||clipped)&&xz_bargein_triggered(b)){fprintf(stderr,"FALSE_TRIGGER delay=%u clipped=%d near_only=%d frame=%d start=%d\n",delay,clipped,near_only,frame,start);assert(!xz_bargein_triggered(b));}
 }
 if(!clipped){assert(first>=start);assert(first-start<=40);int16_t pre[XZ_BARGEIN_PREROLL];assert(xz_bargein_preroll(b,pre,XZ_BARGEIN_PREROLL)==XZ_BARGEIN_PREROLL);int count=0;for(unsigned i=0;i<XZ_BARGEIN_PREROLL;i++)count+=pre[i]!=0;assert(count>1000);}
 assert(allocs==setup_allocs);xz_bargein_destroy(b);assert(live==0);
 printf("delay=%u clipped=%d near_only=%d trigger_frame=%d onset=%d\n",delay,clipped,near_only,first,start);
}
int main(void){
 acoustic_case(0,false,false);acoustic_case(256,false,false);acoustic_case(448,false,false);acoustic_case(0,false,true);acoustic_case(448,true,false);
 for(size_t f=1;f<=3;f++){allocs=0;fail_at=f;assert(!xz_bargein_create(0));assert(live==0);}fail_at=0;
 allocs=0;fail_at=4;assert(!xz_bargein_create(448));assert(live==0);fail_at=0;
 assert(!xz_bargein_create(XZ_BARGEIN_DELAY_MAX+1));assert(!xz_bargein_feed(NULL,NULL,NULL));xz_bargein_destroy(NULL);puts("barge-in signal and lifetime PASS");return 0;
}
