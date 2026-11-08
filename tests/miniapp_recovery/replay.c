/* Compile with the real install_layout.c and an unmodified excerpt from
 * recovery/main/wifi_install.c. Only the SDK/flash/radio layers are simulated. */
#include "install_layout.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX mbedtls_sha256_context;
#define MD5_HASH(p,n,d) CC_MD5((p),(CC_LONG)(n),(d))
#define SHA_HASH(p,n,d) CC_SHA256((p),(CC_LONG)(n),(d))
#define S_INIT CC_SHA256_Init
#define S_UPDATE CC_SHA256_Update
#define S_FINAL CC_SHA256_Final
#else
#include <openssl/md5.h>
#include <openssl/sha.h>
typedef SHA256_CTX mbedtls_sha256_context;
#define MD5_HASH MD5
#define SHA_HASH SHA256
#define S_INIT SHA256_Init
#define S_UPDATE SHA256_Update
#define S_FINAL SHA256_Final
#endif
#define mbedtls_md5(p,n,d) ((void)MD5_HASH(p,n,d))
#define mbedtls_sha256_init(s) ((void)S_INIT(s))
#define mbedtls_sha256_starts(s,mode) ((void)S_INIT(s))
#define mbedtls_sha256_update(s,p,n) ((void)S_UPDATE(s,p,n))
#define mbedtls_sha256_finish(s,d) ((void)S_FINAL(d,s))
#define mbedtls_sha256_free(s) ((void)0)
#define APP_OFFSET 0x10000u
#define MAX_PUBLIC_APP_SIZE 0x600000u
#define GOT_IP_BIT 1
#define xEventGroupGetBits(e) GOT_IP_BIT
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define emit(...) ((void)0)
#define esp_crt_bundle_attach NULL
enum { ESP_OK,ESP_ERR_NO_MEM,ESP_ERR_INVALID_CRC,ESP_ERR_NOT_SUPPORTED,ESP_ERR_INVALID_STATE,
       ESP_ERR_INVALID_ARG,ESP_ERR_INVALID_SIZE,ESP_ERR_INVALID_RESPONSE,ESP_IMAGE_VERIFY };
typedef int esp_err_t;
typedef struct { const char *url;void *crt_bundle_attach;int timeout_ms,buffer_size; } esp_http_client_config_t;
typedef struct { const char *url;uint8_t sha256[32];uint32_t total_size,app_offset; } work_t;
typedef struct { uint32_t offset,size; } esp_partition_pos_t;
typedef int esp_image_metadata_t;
static uint8_t flash[0x800000],*bin,*app,*old,*initial;
static size_t bin_size,app_size,old_size,initial_size,available,cursor,chunk=4096;
static unsigned ops,reboots,verified,status_code=200;
static bool reject_app;
typedef void *esp_http_client_handle_t;
static void *esp_http_client_init(const esp_http_client_config_t *c){assert(c->buffer_size==4096);return &cursor;}
static int esp_http_client_open(void *client,int len){assert(client==&cursor&&!len);cursor=0;return ESP_OK;}
static int esp_http_client_fetch_headers(void *client){(void)client;return (int)bin_size;}
static int esp_http_client_get_status_code(void *client){(void)client;return status_code;}
static int esp_http_client_read(void *client,char *out,size_t n){
    (void)client;if(n>chunk)n=chunk;if(n>available-cursor)n=available-cursor;
    memcpy(out,bin+cursor,n);cursor+=n;return (int)n;
}
static void esp_http_client_cleanup(void *client){(void)client;}
static void esp_restart(void){reboots++;}
static int esp_flash_read(void *chip,void *out,size_t at,size_t n){
    (void)chip;assert(at<=sizeof flash&&n<=sizeof flash-at);memcpy(out,flash+at,n);return ESP_OK;
}
static void allow_change(size_t at,size_t n){
    /* The existing bootloader and permanent network/Recovery partitions must never be erased, even transiently. */
    assert(at>=0x8000&&at<=INSTALL_BACKUP+0x1000&&n<=INSTALL_BACKUP+0x1000-at);ops++;
}
static int esp_flash_erase_region(void *chip,size_t at,size_t n){
    (void)chip;allow_change(at,n);assert(!(at%4096||n%4096));memset(flash+at,0xff,n);return ESP_OK;
}
static int esp_flash_write(void *chip,const void *in,size_t at,size_t n){
    (void)chip;allow_change(at,n);const uint8_t *p=in;
    for(size_t i=0;i<n;i++)assert((flash[at+i]&p[i])==p[i]);
    memcpy(flash+at,in,n);return ESP_OK;
}
static int esp_image_verify(int mode,esp_partition_pos_t *p,esp_image_metadata_t *m){
    (void)m;verified++;assert(mode==ESP_IMAGE_VERIFY&&p->offset==APP_OFFSET&&p->size>=app_size);
    return !reject_app&&!memcmp(flash+p->offset,app,app_size)?ESP_OK:ESP_ERR_INVALID_CRC;
}
#include "recovery_download.inc"
static uint8_t *read_file(const char *path,size_t *n){
    FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long len=ftell(f);assert(len>0);rewind(f);
    *n=(size_t)len;uint8_t *p=malloc(*n);assert(p&&fread(p,1,*n,f)==*n);fclose(f);return p;
}
static void reset_flash(void){
    memset(flash,0xa5,sizeof flash);memcpy(flash+0x8000,initial,INSTALL_TABLE_SIZE);
    memset(flash+INSTALL_JOURNAL,0xff,4096);ops=reboots=verified=0;status_code=200;
    available=bin_size;reject_app=false;
}
static bool marked(void){return memcmp(flash+INSTALL_JOURNAL,"\xff\xff\xff\xff",4)!=0;}
static work_t offer(void){
    work_t w={.url="https://firmware.example.invalid/offer.bin",.total_size=(uint32_t)bin_size,.app_offset=APP_OFFSET};
    SHA_HASH(bin,bin_size,w.sha256);return w;
}
static void verify_contents(void){
    install_layout_t p;assert(table_md5(flash+0x8000,false)&&install_layout_build(bin+0x8000,&p));
    assert(table_md5(p.table,true)&&!memcmp(p.table,flash+0x8000,INSTALL_TABLE_SIZE));
    for(unsigned i=0;i<p.count;i++){
        install_part_t *q=p.parts+i;
        size_t n=q->source<bin_size?bin_size-q->source:0;
        if(n>q->size)n=q->size;
        if(q->copy&&n)assert(!memcmp(flash+q->target,bin+q->source,n));
        for(size_t j=q->copy?n:0;j<q->size;j++)assert(flash[q->target+j]==0xff);
    }
    for(size_t j=0;j<0x8000;j++)assert(flash[j]==0xa5);
    for(size_t j=INSTALL_BACKUP+0x1000;j<sizeof flash;j++)assert(flash[j]==0xa5);
    assert(!marked()&&verified>0);
}
int main(int argc,char **argv){
    assert(argc==6);bin=read_file(argv[1],&bin_size);app=read_file(argv[2],&app_size);
    initial=read_file(argv[3],&initial_size);old=read_file(argv[4],&old_size);
    assert(initial_size>=INSTALL_TABLE_SIZE&&table_md5(initial,false));work_t w=offer();
    reset_flash();status_code=302;assert(download_application(&w)==ESP_ERR_INVALID_RESPONSE&&!ops);
    status_code=200;w.total_size--;assert(download_application(&w)==ESP_ERR_INVALID_RESPONSE&&!ops);w=offer();
    /* Unsupported .17 is rejected before the first flash modification. */
    uint8_t *keep=bin;size_t size=bin_size;bin=old;bin_size=old_size;reset_flash();
    work_t old_offer=offer();assert(download_application(&old_offer)==ESP_ERR_NOT_SUPPORTED&&!ops);
    bin=keep;bin_size=size;reset_flash();
    bin[0x8000+12]^=1;assert(download_application(&w)==ESP_ERR_NOT_SUPPORTED&&!ops);bin[0x8000+12]^=1;
    available=bin_size/2;chunk=1739;
    assert(download_application(&w)==ESP_ERR_INVALID_RESPONSE&&marked()&&!verified);
    assert(!memcmp(flash+INSTALL_BACKUP,initial,INSTALL_TABLE_SIZE));
    memset(flash+0x8000,0,INSTALL_TABLE_SIZE);
    assert(wifi_install_repair_table()==ESP_OK&&reboots==1&&!memcmp(flash+0x8000,initial,INSTALL_TABLE_SIZE));
    available=bin_size;assert(download_application(&w)==ESP_OK);verify_contents();
    reset_flash();chunk=4096;w.sha256[0]^=1;
    assert(download_application(&w)==ESP_ERR_INVALID_CRC&&marked()&&!verified);w.sha256[0]^=1;
    reject_app=true;assert(download_application(&w)==ESP_ERR_INVALID_CRC&&marked()&&verified==1);
    reject_app=false;assert(download_application(&w)==ESP_OK);verify_contents();
    FILE *f=fopen(argv[5],"wb");assert(f&&fwrite(flash,1,sizeof flash,f)==sizeof flash);fclose(f);
    puts("PASS: upstream accepts merged BIN; all installed resource bytes, protected boot/Wi-Fi/Recovery, table integrity, truncated download, failed SHA/image, rejected old image and retry.");
    return 0;
}
