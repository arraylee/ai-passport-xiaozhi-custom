/* Execute the production Flash-resource loader and the exact externalized font
 * sources. Only the partition driver/CRC hardware is substituted on the host. */
#include "lvgl.h"
#include "font_heap_cap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "esp_partition.h"
#include "body_font_meta.h"
LV_FONT_DECLARE(ui_font_body);
LV_FONT_DECLARE(ui_font_title);
static uint8_t *companion_data,*body_data;
static unsigned maps_live,maps_total,fault;
#ifdef PASSPORT_MINIAPP
static esp_partition_t art_part={0x3bb000,0x192000,false,false};
static esp_partition_t font_part={0x2de000,0xdd000,false,false};
#else
static esp_partition_t art_part={0x600000,0x200000,true,false};
static esp_partition_t font_part={0x500000,0x100000,true,false};
#endif
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t *data,uint32_t size){return crc32(crc,data,size);}
const esp_partition_t *esp_partition_find_first(int type,int subtype,const char *name){
    assert(type==ESP_PARTITION_TYPE_DATA);
    if(subtype==0x41){assert(!strcmp(name,"companion"));return fault==1?NULL:&art_part;}
    assert(subtype==0x40&&!strcmp(name,"petart"));return fault==2?NULL:&font_part;
}
int esp_partition_mmap(const esp_partition_t *part,size_t offset,size_t size,int type,
                      const void **out,esp_partition_mmap_handle_t *handle){
    assert(type==ESP_PARTITION_MMAP_DATA);
    unsigned bit=part==&art_part?1:2;
    if(fault==bit+2)return -1;
    assert(!(maps_live&bit));
    if(bit==1){assert(offset==0&&size<=0x200000);*out=companion_data;}
    else {assert(part==&font_part&&offset==BODY_FONT_OFFSET&&size==BODY_FONT_SIZE);*out=body_data;}
    *handle=bit;maps_live|=bit;maps_total++;return 0;
}
void esp_partition_munmap(esp_partition_mmap_handle_t value){assert(maps_live&value);maps_live&=~value;}
#include "../../../components/ui/presentation/src/companion_resources.c"
static uint16_t u16(const uint8_t *p){return p[0]|(uint16_t)p[1]<<8;}
static uint8_t *read_file(const char *path,size_t *size){
    FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>0);rewind(f);
    uint8_t *p=malloc(n);assert(p&&fread(p,1,n,f)==(size_t)n);fclose(f);*size=n;return p;
}
static void failed_init(void){
    assert(!companion_resources_init()&&maps_live==0&&!companion_avatar(0));
    lv_font_glyph_dsc_t g={0};assert(!lv_font_get_glyph_dsc(&ui_font_body,&g,0x54e6,0));
}
static void exercise_resource_failures(void){
    /* Compact resources are authenticated by content and need no readonly flag. */
#ifdef PASSPORT_MINIAPP
    unsigned capacity=art_part.size;art_part.size=COMPANION_SIZE-1;failed_init();art_part.size=capacity;
#else
    failed_init(); /* caller starts with deliberately invalid readonly flag */
    art_part.readonly=true;
#endif
    for(fault=1;fault<=4;fault++)failed_init();fault=0;
    font_part.encrypted=true;failed_init();font_part.encrypted=false;
#ifdef PASSPORT_MINIAPP
    capacity=font_part.size;font_part.size=BODY_FONT_OFFSET+BODY_FONT_SIZE-1;failed_init();font_part.size=capacity;
#else
    font_part.address++;failed_init();font_part.address--;
#endif
    const unsigned positions[]={0,4,8,12,16,24,32,36,63,64,BODY_FONT_SIZE-1};
    for(unsigned i=0;i<sizeof positions/sizeof *positions;i++){
        body_data[positions[i]]^=255;failed_init();body_data[positions[i]]^=255;
    }
    companion_data[128]^=1;failed_init();companion_data[128]^=1;
    assert(companion_resources_init()&&maps_live==3&&companion_avatar(0)&&companion_avatar(1)&&!companion_avatar(2));
    unsigned before=maps_total;assert(companion_resources_init()&&maps_total==before);
}
static unsigned verify_font_range(const lv_font_t *font,const uint8_t *characters,unsigned count,unsigned base){
    unsigned long long ink=0;
    lv_draw_buf_t *buf=lv_draw_buf_create(40,40,LV_COLOR_FORMAT_A8,0);assert(buf);
    for(unsigned i=0;i<count;i++){
        unsigned c=characters?u16(characters+2*i):base+i;
        lv_font_glyph_dsc_t g={0};assert(lv_font_get_glyph_dsc(font,&g,c,0)&&!g.is_placeholder);
        memset(buf->data,0,buf->data_size);assert(lv_font_get_glyph_bitmap(&g,buf)==buf);
        unsigned nonzero=0;for(unsigned y=0;y<g.box_h;y++)for(unsigned x=0;x<g.box_w;x++)nonzero+=buf->data[y*buf->header.stride+x]!=0;
        assert(nonzero || c==0x3164);ink+=nonzero;
    }
    lv_draw_buf_destroy(buf);assert(ink>count);return count;
}
int main(void){
    lv_init();size_t size;
    companion_data=read_file(FONT_COMPANION_FILE,&size);assert(size>=COMPANION_SIZE&&size<=art_part.size);
    body_data=read_file(FONT_BODY_FILE,&size);assert(size==BODY_FONT_SIZE);
    art_part.readonly=false;exercise_resource_failures();
    uint8_t *languages=read_file(FONT_LANG_FILE,&size);passport_languages_bind(languages,size);
    unsigned supplementary=u32(languages+4);
    const lv_font_t *test_fonts[]={&ui_font_body,&ui_font_title};
    for(unsigned i=0;i<2;i++){
        assert(verify_font_range(test_fonts[i],NULL,11172,0xac00)==11172);
        verify_font_range(test_fonts[i],NULL,94,0x3131);
        verify_font_range(test_fonts[i],languages+16,supplementary,0);
    }
    printf("Korean and language fallback: 11172 syllables + 94 letters + %u supplemental characters, both sizes decoded.\n",supplementary);
    uint8_t *oracle=read_file(FONT_ORACLE_FILE,&size);
    assert(!memcmp(oracle,"FTO1",4)&&size==8+16*u32(oracle+4));
    unsigned counts[2]={0},pixels=0,interjections=0;
    for(size_t at=8;at<size;at+=16){
        const uint8_t *r=oracle+at;uint32_t c=u32(r);unsigned native=u16(r+4);
        const lv_font_t *f=native==16?&ui_font_body:&ui_font_title;assert(native==16||native==20);
        lv_font_glyph_dsc_t g={0};
        if(!lv_font_get_glyph_dsc(f,&g,c,0)||g.is_placeholder){fprintf(stderr,"Missing %upx U+%04X\n",native,c);abort();}
        assert(g.adv_w==u16(r+6)&&g.box_w==r[8]&&g.box_h==r[9]&&g.ofs_x==(int8_t)r[10]&&g.ofs_y==(int8_t)r[11]);
        uint32_t crc=0;
        if(g.box_w&&g.box_h){
            lv_draw_buf_t *out=lv_draw_buf_create(g.box_w,g.box_h,LV_COLOR_FORMAT_A8,0);assert(out);
            assert(lv_font_get_glyph_bitmap(&g,out)==out);
            bool visible=false;
            for(unsigned y=0;y<g.box_h;y++){
                crc=crc32(crc,out->data+y*out->header.stride,g.box_w);
                for(unsigned x=0;x<g.box_w;x++)visible|=out->data[y*out->header.stride+x]!=0;
            }
            assert(visible);pixels+=g.box_w*g.box_h;lv_draw_buf_destroy(out);
        }
        if(crc!=u32(r+12)){fprintf(stderr,"Pixel mismatch %upx U+%04X\n",native,c);abort();}
        counts[native==20]++;interjections+=c==0x54e6||c==0x5440||c==0x55ef||c==0x6b38;
        assert(!font_heap_live&&lv_mem_test()==LV_RESULT_OK);
    }
    assert(counts[0]>7000&&counts[0]==counts[1]&&interjections==8&&font_heap_peak<=512);
    lv_font_glyph_dsc_t absent={0};assert(!lv_font_get_glyph_dsc(&ui_font_body,&absent,0x10ffff,0)&&absent.is_placeholder);
    printf("Mapped firmware fonts: %u body + %u title glyphs, %u pixels match independent raw-font oracle; missing/blank/corrupt=0; 哦呀嗯欸 pass; decoder heap <=512B.\n",counts[0],counts[1],pixels);
    printf("Production resource loader: missing/corrupt/wrong partition/read-only/map failure, cleanup and idempotent init PASS.\n");
    passport_bind_body(NULL);passport_bind_nickname(NULL);mapped=NULL;
    esp_partition_munmap(handle);esp_partition_munmap(body_handle);assert(!maps_live);
    free(languages);free(oracle);free(body_data);free(companion_data);return 0;
}
