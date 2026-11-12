/* Production download/cache/reboot regression; network, NVS and Flash are simulated. */
#include "theme_store_stubs.h"
#include "xz_theme.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
static uint8_t raw[XZ_THEME_LIMIT],flash[0x800000],before[0x800000];
static esp_partition_t theme={0x41a000,XZ_THEME_LIMIT},cache={0x5e9000,XZ_CACHE_LIMIT};
static size_t bytes,pos,available;
static unsigned maps,cache_writes,erases,opens;
static bool readonly_fail,fail_cache;
static char url[770],mode[24];
static const char *test_url="https://example.org/assets.bin";
static int persisted_dark=-1;
static int is(const char *s){return !strcmp(mode,s);}
int esp_crt_bundle_attach(void *p){(void)p;return 0;}
const esp_partition_t *esp_partition_find_first(int t,int st,const char *name){
 assert(t==1);if(st==130&&!strcmp(name,"assets"))return &theme;
 assert(st==0x42&&!strcmp(name,"theme_cache"));return &cache;
}
int esp_partition_read(const esp_partition_t *p,size_t at,void *out,size_t n){assert(at<=p->size&&n<=p->size-at);memcpy(out,p->bytes+at,n);return 0;}
int esp_partition_mmap(const esp_partition_t *p,size_t at,size_t n,int type,const void **out,unsigned *h){assert(at<=p->size&&n<=p->size-at&&type==0);*h=++maps;*out=p->bytes+at;return 0;}
void esp_partition_munmap(unsigned h){assert(h>0&&maps>0);maps--;}
int esp_partition_write(const esp_partition_t *p,size_t at,const void *data,size_t n){
 assert(!p->readonly&&at<=p->size&&n<=p->size-at);if(p==&cache){cache_writes++;if(fail_cache)return -1;}
 const uint8_t *in=data;for(size_t i=0;i<n;i++){assert(!(in[i]&~p->bytes[at+i]));p->bytes[at+i]&=in[i];}return 0;
}
int esp_partition_erase_range(const esp_partition_t *p,size_t at,size_t n){assert(!p->readonly&&!(at%4096)&&!(n%4096)&&at<=p->size&&n<=p->size-at);memset(p->bytes+at,255,n);erases++;return 0;}
unsigned esp_rom_crc32_le(unsigned crc,const void *p,size_t n){return crc32(crc,p,n);}
int nvs_open(const char *s,int m,int *h){assert(!strcmp(s,"xz_theme"));*h=m+1;return 0;}
int nvs_get_u8(int h,const char *s,uint8_t *out){assert(!strcmp(s,"dark"));if(persisted_dark<0)return -1;*out=persisted_dark;return 0;}
int nvs_set_u8(int h,const char *s,uint8_t v){assert(h==2&&!strcmp(s,"dark"));persisted_dark=v;return 0;}
void nvs_close(int h){assert(h);}
int nvs_commit(int h){assert(h==2);return 0;}
int nvs_get_str(int h,const char *s,char *out,size_t *n){assert(!strcmp(s,"url"));size_t len=strlen(url)+1;if(len<2)return -1;if(out){assert(*n>=len);memcpy(out,url,len);}*n=len;return 0;}
int nvs_set_str(int h,const char *s,const char *v){assert(h==2&&!strcmp(s,"url")&&strlen(v)<sizeof url);strcpy(url,v);return 0;}
int nvs_erase_key(int h,const char *s){assert(h==2&&!strcmp(s,"url"));url[0]=0;return 0;}
void vTaskDelay(int n){assert(n==1);}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg){assert(!strcmp(cfg->url,test_url)&&cfg->disable_auto_redirect&&cfg->crt_bundle_attach);return (void*)1;}
int esp_http_client_open(esp_http_client_handle_t p,int n){assert(p&&n==0);opens++;return is("offline")?-1:0;}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t p){return is("oversize-download")?XZ_THEME_LIMIT+1:0;}
int esp_http_client_get_status_code(esp_http_client_handle_t p){return opens==1?302:200;}
int esp_http_client_set_redirection(esp_http_client_handle_t p){return 0;}
int esp_http_client_close(esp_http_client_handle_t p){return 0;}
int esp_http_client_cleanup(esp_http_client_handle_t p){return 0;}
int esp_http_client_read(esp_http_client_handle_t p,char *out,size_t n){if(pos>=available)return 0;if(n>available-pos)n=available-pos;if(n>583)n=583;memcpy(out,raw+pos,n);pos+=n;return n;}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t p){return pos==bytes;}
static void disk(const char *path,uint8_t *p,size_t n,bool write){FILE *fp=fopen(path,write?"wb":"rb");assert(fp);assert((write?fwrite(p,1,n,fp):fread(p,1,n,fp))==n);fclose(fp);}
int main(int argc,char **argv){
 assert(argc==6);unsigned expected_edge=atoi(argv[3]),expected_count=atoi(argv[4]),expected_display=atoi(argv[5]);
 strcpy(mode,argv[1]);FILE *f=fopen(argv[2],"rb");assert(f);bytes=fread(raw,1,sizeof raw,f);fclose(f);available=bytes;
 theme.bytes=flash+theme.address;cache.bytes=flash+cache.address;memset(flash,0xa5,sizeof flash);
 memset(theme.bytes,255,theme.size);memset(cache.bytes,255,cache.size);
 bool cached_boot=is("warm")||is("cache-corrupt")||is("old-cache")||is("bad-edge")||is("broken-pack")||is("bad-chip");
 if(cached_boot){disk("fixtures/store-theme.flash",theme.bytes,theme.size,false);disk("fixtures/store-cache.flash",cache.bytes,cache.size,false);}
 if(is("broken-pack"))theme.bytes[bytes-1]^=1;
 if(is("cache-corrupt"))cache.bytes[1000]^=1;
 if(is("old-cache"))cache.bytes[3]=0x31;
 if(is("bad-edge"))cache.bytes[16]=255;
 if(is("bad-chip"))theme.readonly=true;
 if(!cached_boot&&!is("blank"))strcpy(url,test_url);
 if(is("power-loss"))available=bytes/2;
 if(is("bad-index")){raw[12+36]=1;unsigned sum=0;for(size_t i=12;i<bytes;i++)sum+=raw[i];raw[4]=sum;raw[5]=sum>>8;}
 memcpy(before,flash,sizeof before);xz_theme_boot();
 unsigned delay=0;
 if(is("warm")||is("cache-corrupt")||is("old-cache")||is("bad-edge")){
    assert(xz_theme_installed()&&maps==2);assert(is("warm")?cache_writes==0:cache_writes>0);
    const uint8_t *a=xz_theme_icon(0,0,&delay);assert(a&&delay==0);
    for(unsigned e=0;e<XZ_EMOTIONS;e++){
      const uint8_t *p=xz_theme_icon(e,900,&delay);assert(p&&delay==0);
      assert((p==a)==(e==0||expected_count==1||e>=expected_count));
      unsigned size=0;assert(xz_theme_icon_edge(e,&size)==expected_edge&&size==expected_display);
    }
    /* One uploaded file shared by every emotion must occupy one cache frame. */
    assert(xz_u32(cache.bytes+8)==expected_count&&xz_u32(cache.bytes+16)==expected_edge);

 }else if(!cached_boot&&!is("blank")){
    assert(xz_theme_pending_boot()&&!xz_theme_installed()&&!maps);
    if(is("cancel"))xz_theme_cancel_download();
    atomic_bool stop=false;fail_cache=is("cache-fail");assert(xz_theme_download(&stop)==!!is("receive"));
    if(is("offline")||is("power-loss")||is("cancel"))assert(strstr(xz_theme_last_error(),"检查网络后重试"));
    if(is("oversize-download")){assert(strstr(xz_theme_last_error(),"920KiB"));assert(!pos&&!erases&&!cache_writes);}
    if(is("oversize-png")||is("bad-index")||is("cache-fail"))assert(strstr(xz_theme_last_error(),"128×128"));
    assert(!xz_theme_pending_boot()&&!url[0]&&!maps);
    if(is("receive")){assert(!memcmp(raw,theme.bytes,bytes)&&cache_writes==1+expected_count*(1+2*expected_edge*expected_edge/1024));assert(xz_u32(cache.bytes+8)==expected_count);disk("fixtures/store-theme.flash",theme.bytes,theme.size,true);disk("fixtures/store-cache.flash",cache.bytes,cache.size,true);}
    else assert(xz_u32(theme.bytes)==0xffffffffu);
 }else assert(!xz_theme_installed()&&!maps&&!erases);
 if(!is("bad-chip")){
   assert(xz_theme_dark()&&xz_theme_set_dark(false)&&!xz_theme_dark()&&persisted_dark==0);
   assert(!xz_theme_queue_url("ftp://assets")&&!xz_theme_queue_url("https:///missing")&&!xz_theme_queue_url("https://a/\nb"));
   assert(xz_theme_queue_url(test_url)&&!strcmp(url,test_url)&&xz_theme_queue_reboot()&&xz_theme_reboot_requested());
 }
 for(size_t i=0;i<sizeof flash;i++)if((i<theme.address||i>=theme.address+theme.size)&&(i<cache.address||i>=cache.address+cache.size))assert(before[i]==flash[i]);
 printf("Flash/HTTP boot scenario %s passed (%u erases, %u cache writes)\n",mode,erases,cache_writes);
}
