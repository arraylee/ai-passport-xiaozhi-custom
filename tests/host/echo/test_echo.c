#include "xz_echo.h"
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t live,allocs,fail_at;
void *qa_malloc(size_t size){if(++allocs==fail_at)return NULL;void*p=malloc(size);if(p)live++;return p;}
void *qa_calloc(size_t n,size_t size){if(++allocs==fail_at)return NULL;void*p=calloc(n,size);if(p)live++;return p;}
void qa_free(void*p){if(p){assert(live);live--;free(p);}}
static uint32_t random_state=1234567;
static int noise(void){random_state=random_state*1664525u+1013904223u;return (int)(random_state>>16)-32768;}
static void resources(void){
 for(size_t failure=1;failure<=2;failure++){allocs=0;fail_at=failure;assert(!xz_echo_create());assert(live==0);}
 fail_at=0;
 for(size_t bytes=1;bytes<XZ_ECHO_POOL_BYTES;bytes+=127){xz_echo_t*s=xz_echo_test_create(bytes);xz_echo_destroy(s);assert(live==0);}
 for(int cycle=0;cycle<40;cycle++){
  xz_echo_t*s=xz_echo_create();assert(s);assert(xz_echo_workspace_used(s)<=XZ_ECHO_POOL_BYTES);
  size_t created=allocs;int16_t zero[64]={0},out[64];
  for(int f=0;f<20;f++){assert(xz_echo_process(s,zero,zero,out));for(int i=0;i<64;i++)assert(out[i]==0);}
  assert(xz_echo_reset(s));assert(allocs==created);xz_echo_destroy(s);assert(live==0);
 }
 assert(!xz_echo_process(NULL,NULL,NULL,NULL));assert(!xz_echo_reset(NULL));xz_echo_destroy(NULL);
}
static void signals(void){
 xz_echo_t*s=xz_echo_create();assert(s);size_t created=allocs;
 int16_t hist[4096]={0},mic[64],ref[64],clean[64],near[64];int cursor=0,smooth=0;
 double echo_in=0,echo_out=0,near_energy=0,near_error=0;
 for(int frame=0;frame<2000;frame++){
  for(int i=0;i<64;i++){
   smooth=(3*smooth+noise()/3)/4;ref[i]=(int16_t)smooth;hist[cursor&4095]=ref[i];
   int echo=hist[(cursor-40)&4095]/2+hist[(cursor-81)&4095]/4+hist[(cursor-170)&4095]/8;
   near[i]=frame>=1500?(int16_t)(noise()/8):0;mic[i]=(int16_t)(echo+near[i]);cursor++;
  }
  assert(xz_echo_process(s,mic,ref,clean));
  for(int i=0;i<64;i++){
   if(frame>=1000&&frame<1500){echo_in+=(double)mic[i]*mic[i];echo_out+=(double)clean[i]*clean[i];}
   if(frame>=1600){double e=clean[i]-near[i];near_error+=e*e;near_energy+=(double)near[i]*near[i];}
  }
 }
 assert(echo_out<echo_in/30.0); /* >14.7dB, independent delayed multipath signal */
 assert(near_error<near_energy/3.0); /* near-end remains recoverable during echo */
 printf("pool=%zu echo_reduction_db=%.2f near_snr_db=%.2f\n",xz_echo_workspace_used(s),10*log10(echo_in/echo_out),10*log10(near_energy/near_error));
 assert(allocs==created);assert(xz_echo_reset(s));
 for(int frame=0;frame<300;frame++){
  for(int i=0;i<64;i++){ref[i]=frame%2?32767:-32768;mic[i]=i%2?32767:-32768;}
  assert(xz_echo_process(s,mic,ref,clean));
 }
 assert(xz_echo_reset(s));memset(mic,0,sizeof mic);memset(ref,0,sizeof ref);
 assert(xz_echo_process(s,mic,ref,clean));for(int i=0;i<64;i++)assert(clean[i]==0);
 xz_echo_destroy(s);assert(live==0);
}
int main(void){resources();signals();puts("echo resource/signal/reset/clipping checks PASS");return 0;}
