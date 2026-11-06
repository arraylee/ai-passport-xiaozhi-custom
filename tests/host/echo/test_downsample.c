#include "xz_downsample.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static double tone(unsigned rate,unsigned hz){xz_downsample_t s;assert(xz_downsample_init(&s,rate));double power=0;unsigned n=0;for(unsigned i=0;i<rate;i++){int16_t a,b,x=(int16_t)(12000*sin(6.283185307179586*hz*i/rate));if(xz_downsample_push(&s,x,x,&a,&b)){assert(a==b);if(i>100){power+=(double)a*a;n++;}}}return sqrt(power/n);}
int main(void){xz_downsample_t s;assert(!xz_downsample_init(&s,44100));for(unsigned rate=16000;rate<=24000;rate+=8000){assert(xz_downsample_init(&s,rate));unsigned n=0;int16_t a=0,b=0;for(unsigned i=0;i<rate;i++)if(xz_downsample_push(&s,32767,-32768,&a,&b))n++;assert(n==8000&&a>=32760&&b<=-32760);double pass=tone(rate,1000),stop=tone(rate,6000);printf("rate=%u pass=%.2f stop=%.2f\n",rate,pass,stop);assert(pass>7500&&stop<250);}puts("stream rate, full-scale and anti-alias checks PASS");}
