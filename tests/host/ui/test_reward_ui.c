#include "overlay_test_display.h"
#include "presentation/ui_reward.h"
#include <limits.h>

static unsigned image_count(lv_obj_t *object) {
    unsigned count = 0;
    if (lv_obj_check_type(object, &lv_image_class)) {
        const lv_image_dsc_t *source = lv_image_get_src(object);
        assert(source && source->header.magic == LV_IMAGE_HEADER_MAGIC);
        assert(source->header.cf == LV_COLOR_FORMAT_ARGB8888);
        assert(source->header.w && source->header.h && source->data_size && source->data);
        ++count;
    }
    for (unsigned i=0; i<lv_obj_get_child_count(object); ++i) count += image_count(lv_obj_get_child(object,i));
    return count;
}
static void check_reward(lv_obj_t *home, unsigned old_children, bool add, const char *badge) {
    assert(ui_reward_is_active()); assert(lv_screen_active() == home);
    lv_obj_t *top = lv_layer_top(); assert(lv_obj_get_child_count(top) == old_children+1);
    lv_obj_t *root = lv_obj_get_child(top, -1); overlay_fullscreen(root);
    assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_CLICKABLE));
    assert(!lv_obj_has_flag(root, LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t *label = overlay_find_label(root,badge); assert(label);
    assert(lv_color_eq(lv_obj_get_style_text_color(label,0),lv_color_hex(add ? 0x39FF88 : 0xFF5A5A)));
    assert(image_count(root) == (add ? 1u : 2u));
    lv_area_t badge_area;lv_obj_get_coords(label,&badge_area);
    assert(badge_area.x1>=0&&badge_area.x2<240&&badge_area.y1>=0&&badge_area.y2<320);
}
static void collect_images(lv_obj_t *object,lv_obj_t **images,unsigned *count) {
    if(lv_obj_check_type(object,&lv_image_class)){assert(*count<2);images[(*count)++]=object;}
    for(unsigned i=0;i<lv_obj_get_child_count(object);i++)collect_images(lv_obj_get_child(object,i),images,count);
}
static void assert_art_painted(lv_obj_t *root) {
    uint16_t painted[240*320];memcpy(painted,overlay_frame,sizeof painted);
    lv_obj_t *images[2];unsigned count=0;collect_images(root,images,&count);assert(count);
    // Same tick/animation state, only toggle image visibility: verifies real
    // immutable ARGB art contributes pixels, not just the scrim/badge.
    for(unsigned i=0;i<count;i++)lv_obj_add_flag(images[i],LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);assert(memcmp(painted,overlay_frame,sizeof painted));
    for(unsigned i=0;i<count;i++)lv_obj_remove_flag(images[i],LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(NULL);assert(!memcmp(painted,overlay_frame,sizeof painted));
}
int main(void) {
    lv_obj_t *home=overlay_setup(), *top=lv_layer_top();
    uint16_t baseline[240*320]; memcpy(baseline,overlay_frame,sizeof baseline);
    unsigned children=lv_obj_get_child_count(top), timers=overlay_timer_count(), animations=lv_anim_count_running();
    assert(!ui_reward_is_active());
    ui_reward_play(true,7); check_reward(home,children,true,"+7");
    overlay_step(300); check_reward(home,children,true,"+7");
    assert_art_painted(lv_obj_get_child(top,-1));
    assert(memcmp(baseline,overlay_frame,sizeof baseline)); // Real overlay actually paints.
    overlay_step(400); overlay_step(650); // Exercise spinning image and whole-root fade boundary.
    assert(ui_reward_is_active()); overlay_step(249); assert(ui_reward_is_active());
    overlay_step(1); assert(!ui_reward_is_active());
    assert(lv_obj_get_child_count(top)==children && lv_anim_count_running()==animations && overlay_timer_count()==timers);
    assert(!memcmp(baseline,overlay_frame,sizeof baseline));

    // Replace while active: old deadline/coin/label must not remove the new gift.
    ui_reward_play(true,11); overlay_step(900);
    ui_reward_play(false,23); check_reward(home,children,false,"-23");
    assert(!overlay_find_label(top,"+11"));
    overlay_step(700); assert(ui_reward_is_active()); // Original +11 expires now.
    assert_art_painted(lv_obj_get_child(top,-1));
    overlay_step(899); check_reward(home,children,false,"-23");
    overlay_step(1); assert(!ui_reward_is_active());
    assert(lv_anim_count_running()==animations && overlay_timer_count()==timers);

    const int values[]={0,-1,INT_MAX};
    for (unsigned i=0;i<3;i++) for(unsigned add=0;add<2;add++) {
        char expected[24];
        if(values[i]>0)snprintf(expected,sizeof expected,"%s%d",add?"+":"-",values[i]);
        else snprintf(expected,sizeof expected,"%s",add?"+":"-");
        ui_reward_play(add,values[i]);check_reward(home,children,add,expected);
        overlay_step(400);overlay_step(1200);assert(!ui_reward_is_active());
    }
    // Warm allocator paths, then repeated add/sub/active-replacement must return
    // live allocations, timers and animations, not merely show the final screen.
    lv_mem_monitor_t warm,last;
    for(unsigned i=0;i<30;i++) {
        ui_reward_play(i%2==0,(int)i+1);overlay_step(320);
        if(i%3==0){ui_reward_play(false,9);overlay_step(320);}
        overlay_step(1600);
        assert(!ui_reward_is_active()&&lv_screen_active()==home);
        assert(lv_obj_get_child_count(top)==children&&overlay_timer_count()==timers&&lv_anim_count_running()==animations);
        lv_mem_monitor(&last);if(i==4)warm=last;
        if(i>4){assert(last.used_cnt==warm.used_cnt);assert(last.total_size-last.free_size<=warm.total_size-warm.free_size);}
    }
    assert(!memcmp(baseline,overlay_frame,sizeof baseline));
    printf("reward UI PASS: real ARGB art, signed badge/zero/bounds, rendered add/sub, replacement, 1600ms close, 30-cycle object/timer/animation/memory recovery; peak=%zu\n",last.max_used);
}
