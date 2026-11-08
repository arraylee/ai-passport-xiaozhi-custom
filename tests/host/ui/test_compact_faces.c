/* Compare the completed LCD image, with normal and clipped positions, not just
 * compressor round trips. Use the actual native LVGL renderer and 8-row buffers. */
#include "presentation/compact_face.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W=200, H=190, SIDE=XZ_FACE_WIDTH, ROWS=XZ_FACE_ROWS };
static uint16_t lcd[H][W], saved[H][W], buffer[8][W];
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *pixels){
    unsigned n=(unsigned)(a->x2-a->x1+1)*2;
    for(int y=a->y1;y<=a->y2;y++,pixels+=n) memcpy(lcd[y]+a->x1,pixels,n);
    lv_display_flush_ready(d);
}
static unsigned u32(const uint8_t *p){
    return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static uint8_t *readpack(const char *path,unsigned ver,size_t *size){
    FILE *f=fopen(path,"rb");assert(f&&!fseek(f,0,SEEK_END));long n=ftell(f);rewind(f);
    uint8_t *p=malloc(n);assert(p&&fread(p,1,n,f)==(size_t)n);fclose(f);
    assert(!memcmp(p,"CPR1",4)&&u32(p+4)==ver);*size=n;return p;
}
static lv_image_dsc_t frame(const uint8_t *pack,size_t capacity,unsigned i,bool compressed){
    unsigned at=u32(pack+32+i*8),n=u32(pack+36+i*8); assert(at<=capacity&&n<=capacity-at);
    return (lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.w=SIDE,.h=SIDE,.stride=2*SIDE,
        .cf=compressed?LV_COLOR_FORMAT_RAW_ALPHA:LV_COLOR_FORMAT_RGB565A8,.flags=compressed?LV_IMAGE_FLAGS_USER1:0},
        .data=pack+at,.data_size=n};
}
static void render(lv_obj_t *image,lv_image_dsc_t *src,lv_display_t *display,int x,int y){
    lv_image_set_src(image,src);lv_obj_set_pos(image,x,y);
    lv_obj_invalidate(lv_screen_active());lv_refr_now(display);
}
int main(int argc,char **argv){
    assert(argc==3);size_t n1,n2;uint8_t *original=readpack(argv[1],3,&n1),*compact=readpack(argv[2],4,&n2);
    lv_init();assert(compact_face_register());
    lv_display_t *d=lv_display_create(W,H);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d,buffer,NULL,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);lv_obj_t *screen=lv_screen_active();
    lv_obj_clear_flag(screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_bg_color(screen,lv_color_hex(0x355baa),0);
    lv_obj_t *image=lv_image_create(screen);
    lv_image_dsc_t raw[12],packed[12]; uint8_t strip[XZ_FACE_STRIP_BYTES];
    for(unsigned i=0;i<12;i++){
        raw[i]=frame(original,n1,i,false);packed[i]=frame(compact,n2,i,true);
        assert(raw[i].data_size==SIDE*SIDE*3);
        for(unsigned j=0;j<SIDE/ROWS;j++){
            assert(compact_face_decode(packed[i].data,packed[i].data_size,j,strip));
            assert(!memcmp(strip,raw[i].data+j*SIDE*ROWS*2,SIDE*ROWS*2));
            assert(!memcmp(strip+SIDE*ROWS*2,raw[i].data+2*SIDE*SIDE+j*SIDE*ROWS,SIDE*ROWS));
        }
        assert(!compact_face_decode(packed[i].data,packed[i].data_size-1,SIDE/ROWS-1,strip));
        for(unsigned clip=0;clip<2;clip++){
            int x=clip?-17:25,y=clip?-11:23;
            render(image,&raw[i],d,x,y);memcpy(saved,lcd,sizeof lcd);
            unsigned ink=0;for(unsigned row=0;row<H;row++)for(unsigned col=0;col<W;col++)ink+=saved[row][col]!=saved[H-1][W-1];
            assert(ink>1000);render(image,&packed[i],d,x,y);
            assert(!memcmp(saved,lcd,sizeof lcd));
        }
    }
    lv_display_delete(d);free(original);free(compact);
    puts("12 frames, 248832 source pixels; full and clipped LCD images match, including transparency.");
    return 0;
}
