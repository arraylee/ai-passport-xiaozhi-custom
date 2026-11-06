#include "presentation/ui_boot.h"
#include "presentation/ui_xiaozhi.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t buffer[240*20],screen[240*320];
static void flush(lv_display_t*d,const lv_area_t*a,uint8_t*p){
 const uint16_t*src=(const uint16_t*)p;
 for(int y=a->y1;y<=a->y2;y++)for(int x=a->x1;x<=a->x2;x++)screen[y*240+x]=*src++;
 lv_display_flush_ready(d);
}
static void step(unsigned ms){lv_tick_inc(ms);lv_timer_handler();lv_refr_now(NULL);}
static void ppm(const char*dir,unsigned n){
 char name[1024];snprintf(name,sizeof name,"%s/frame-%03u.ppm",dir,n);FILE*f=fopen(name,"wb");assert(f);fprintf(f,"P6\n240 320\n255\n");
 for(unsigned i=0;i<240*320;i++){uint16_t p=screen[i];unsigned char c[3]={(p>>11)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};assert(fwrite(c,1,3,f)==3);}fclose(f);
}
static void dark_corners(void){
 unsigned corners[]={0,239,240*319,240*320-1};
 for(unsigned i=0;i<4;i++){uint16_t p=screen[corners[i]];assert((p>>11)<=1&&((p>>5)&63)<=2&&(p&31)<=2);}
}
int main(int argc,char**argv){
 lv_init();lv_display_t*d=lv_display_create(240,320);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);
 lv_display_set_buffers(d,buffer,NULL,sizeof buffer,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
 lv_obj_t*home=lv_screen_active();unsigned before=lv_obj_get_child_count(lv_layer_top());
 assert(ui_boot_open()&&ui_boot_open());assert(!ui_boot_drawn());
 for(unsigned frame=0;frame<50;frame++){step(33);assert(ui_boot_drawn());dark_corners();if(argc==2)ppm(argv[1],frame);}
 unsigned pink=0;for(unsigned i=0;i<240*320;i++)pink+=(screen[i]>>11)>15&&((screen[i]>>5)&63)<20;assert(pink>4000);
 xz_view_t view={.phase=XZ_BOOT};ui_xiaozhi_open(&view,NULL);step(33);dark_corners();
 ui_boot_close();ui_boot_close();ui_xiaozhi_close();step(40);assert(lv_screen_active()==home&&lv_obj_get_child_count(lv_layer_top())==before);
 lv_mem_monitor_t initial,last;lv_mem_monitor(&initial);
 for(unsigned cycle=0;cycle<100;cycle++){
  assert(ui_boot_open());step(cycle%2?90:950);dark_corners();ui_boot_close();step(40);assert(lv_mem_test()==LV_RESULT_OK);
 }
 lv_mem_monitor(&last);assert(initial.free_size==last.free_size&&initial.used_cnt==last.used_cnt);
 printf("Boot overlay: dark corners, Flash masks, 100 interrupted/completed lifetimes; LVGL peak %u bytes.\n",(unsigned)last.max_used);
 return 0;
}
