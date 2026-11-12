/* ASan/UBSan driver for the production PNG/GIF decoder, with allocation failure injection. */
#include "xz_theme_image.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union { max_align_t align; size_t size; } allocation_t;
static unsigned attempts, fail_at, frames;
static size_t live, peak;
static unsigned edge=32;
void *png_test_malloc(size_t n) {
  if (++attempts == fail_at) return NULL;
  allocation_t *p = malloc(sizeof *p+n);
  assert(p);p->size=n;live+=n;if(live>peak)peak=live;return p+1;
}
void *png_test_calloc(size_t n,size_t size) {
  assert(!n || size<=SIZE_MAX/n);
  void *p=png_test_malloc(n*size);if(p)memset(p,0,n*size);return p;
}
void png_test_free(void *ptr) {
  if(!ptr)return;
  allocation_t *p=(allocation_t *)ptr-1;assert(live>=p->size);live-=p->size;free(p);
}
static bool save(void *ctx,const xz_rgba_t *p,unsigned delay) {
  (void)delay;frames++;
  return fwrite(p,sizeof *p,edge*edge,ctx)==edge*edge;
}
int main(int argc,char **argv) {
  assert(argc>=4 && argc<=6);
  fail_at=argc>=5?(unsigned)atoi(argv[4]):0;
  if(argc==6)edge=(unsigned)atoi(argv[5]);
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);assert(n>0);
  uint8_t *b=malloc(n);assert(b);assert(fread(b,1,n,f)==(size_t)n);fclose(f);
  FILE *out=fopen(argv[2],"wb");assert(out);
  bool ok=edge==32?xz_theme_decode_image((xz_blob_t){b,(size_t)n},12,save,out)
                   :xz_theme_decode_png((xz_blob_t){b,(size_t)n},edge,save,out);
  fclose(out);free(b);
  assert(!live);
  assert(ok==(bool)atoi(argv[3]));
  if(ok)assert(frames);
  else assert(!frames); /* A PNG is not published before its full stream validates. */
  printf("accepted=%u frames=%u allocations=%u peak=%zu\n",ok,frames,attempts,peak);
  return 0;
}
