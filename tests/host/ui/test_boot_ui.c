#include "presentation/ui_boot.h"
#include "presentation/ui_xiaozhi.h"
#include "lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint16_t buffer[240*20],screen[240*320];
static unsigned home_draws;
static void count_home_draw(lv_event_t*e){(void)e;home_draws++;}
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
 lv_obj_add_event_cb(home,count_home_draw,LV_EVENT_DRAW_POST,NULL);step(33);unsigned home_before=home_draws;
 assert(ui_boot_open()&&ui_boot_open());assert(!ui_boot_drawn());
 assert(lv_obj_get_style_opa_layered(home,0)==LV_OPA_TRANSP);
 lv_obj_t*overlay=lv_obj_get_child(lv_layer_top(),-1);
 lv_obj_t*logo=lv_obj_get_child(overlay,0);
 int initial_y=lv_obj_get_style_y(logo,0),previous_y=initial_y;
 // A slow first display flush must not consume the beginning of the animation.
 lv_tick_inc(1000);
 for(unsigned frame=0;frame<104;frame++){
  step(33);assert(ui_boot_drawn());dark_corners();
  if(frame==0){assert(lv_obj_get_y(logo)==initial_y);assert(lv_obj_get_style_image_opa(logo,0)==LV_OPA_COVER);}
  int y=lv_obj_get_y(logo);assert(y>=previous_y&&y<=144&&lv_obj_get_x(logo)==48);previous_y=y;
  assert(home_draws==home_before);
  if(argc==2)ppm(argv[1],frame);
 }
 assert(previous_y==144);
 unsigned pink=0;for(unsigned i=0;i<240*320;i++)pink+=(screen[i]>>11)>15&&((screen[i]>>5)&63)<20;assert(pink>1500);
 // The settled boot view contains only the centered wordmark, no labels or icon.
 for(unsigned y=0;y<320;y++)for(unsigned x=0;x<240;x++)
  if(x<48||x>=192||y<144||y>=175)assert(screen[y*240+x]==0);
 assert(lv_obj_get_child_count(overlay)==1);
 assert(lv_obj_check_type(logo,&lv_image_class));
 xz_view_t view={.phase=XZ_BOOT};ui_xiaozhi_open(&view,NULL);step(33);dark_corners();
 ui_boot_close();ui_boot_close();ui_xiaozhi_close();step(40);assert(lv_screen_active()==home&&lv_obj_get_child_count(lv_layer_top())==before);
 assert(lv_obj_get_style_opa_layered(home,0)==LV_OPA_COVER&&home_draws>home_before);
 lv_mem_monitor_t initial,last;lv_mem_monitor(&initial);
 for(unsigned cycle=0;cycle<100;cycle++){
  assert(ui_boot_open());step(33);assert(ui_boot_drawn());
  step(cycle%2?90:3000);dark_corners();ui_boot_close();step(40);assert(lv_mem_test()==LV_RESULT_OK);
 }
 // Reallocation may recover an allocator header; neither live blocks nor
 // allocated space may grow across the 100 complete/interrupted lifetimes.
 lv_mem_monitor(&last);assert(last.free_size>=initial.free_size&&initial.used_cnt==last.used_cnt);
 printf("Boot overlay: dark corners, Flash masks, 100 interrupted/completed lifetimes; LVGL peak %u bytes.\n",(unsigned)last.max_used);
 return 0;
}
