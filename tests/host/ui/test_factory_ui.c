#include "overlay_test_display.h"
#include "presentation/ui_factory.h"

static void check_pattern(lv_obj_t *home,unsigned baseline_children) {
    assert(lv_screen_active()==home);
    lv_obj_t *top=lv_layer_top();assert(lv_obj_get_child_count(top)==baseline_children+1);
    lv_obj_t *root=lv_obj_get_child(top,-1);overlay_fullscreen(root);
    assert(lv_obj_get_child_count(root)==33); // Five bands plus real 20px grid.
    const uint32_t colors[]={0xff0000,0x00ff00,0x0000ff,0xffffff,0x000000};
    for(unsigned i=0;i<5;i++) {
        lv_obj_t *band=lv_obj_get_child(root,(int)i);
        assert(lv_obj_get_y(band)==(int)i*64 && lv_obj_get_width(band)==240);
        assert(lv_color_eq(lv_obj_get_style_bg_color(band,0),lv_color_hex(colors[i])));
        assert(overlay_frame[(i*64+10)*240+10]==lv_color_to_u16(lv_color_hex(colors[i])));
    }
    for(unsigned i=5;i<33;i++) {
        lv_obj_t *grid=lv_obj_get_child(root,(int)i);
        assert(lv_obj_get_style_bg_opa(grid,0)==LV_OPA_50);
        if(i<17){assert(lv_obj_get_width(grid)==1 && lv_obj_get_height(grid)==320);assert(lv_obj_get_x(grid)==(int)(i-5)*20);}
        else {assert(lv_obj_get_width(grid)==240 && lv_obj_get_height(grid)==1);assert(lv_obj_get_y(grid)==(int)(i-17)*20);}
    }
    // Grid is painted, not only present as an object/style declaration.
    assert(overlay_frame[10*240]!=overlay_frame[10*240+10]);
    assert(overlay_frame[10]!=overlay_frame[10*240+10]);
}
int main(void) {
    lv_obj_t *home=overlay_setup(),*top=lv_layer_top();
    uint16_t original[240*320];memcpy(original,overlay_frame,sizeof original);
    unsigned children=lv_obj_get_child_count(top), timers=overlay_timer_count();
    ui_factory_disp_test();overlay_step(0);check_pattern(home,children);
    overlay_step(14999);check_pattern(home,children);
    overlay_step(1);
    assert(lv_obj_get_child_count(top)==children&&overlay_timer_count()==timers&&lv_screen_active()==home);
    assert(!memcmp(original,overlay_frame,sizeof original));
    // Restart at ten seconds: only one overlay/timer and a fresh full hold.
    ui_factory_disp_test();overlay_step(10000);
    ui_factory_disp_test();overlay_step(5000);check_pattern(home,children);
    assert(overlay_timer_count()==timers+1);
    overlay_step(9999);check_pattern(home,children);
    overlay_step(1);assert(lv_obj_get_child_count(top)==children&&overlay_timer_count()==timers);
    lv_mem_monitor_t warm,last;
    for(unsigned i=0;i<35;i++) {
        ui_factory_disp_test();overlay_step(1);check_pattern(home,children);
        if(i%3==0){ui_factory_disp_test();overlay_step(1);check_pattern(home,children);}
        overlay_step(15000);
        assert(lv_obj_get_child_count(top)==children&&overlay_timer_count()==timers);
        assert(lv_screen_active()==home&&!memcmp(original,overlay_frame,sizeof original));
        lv_mem_monitor(&last);if(i==4)warm=last;
        if(i>4){assert(last.used_cnt==warm.used_cnt);assert(last.total_size-last.free_size<=warm.total_size-warm.free_size);}
    }
    printf("factory UI PASS: actual five-band RGB565 pixels, 28 grid lines, retained screen, 15000ms hold, restart deadline, 35-cycle object/timer/memory recovery; peak=%zu\n",last.max_used);
}
