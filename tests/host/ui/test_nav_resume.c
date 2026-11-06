#include "lvgl.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

static uint16_t buffer[240 * 20];
static lv_indev_t *input;
static lv_indev_state_t physical_state = LV_INDEV_STATE_RELEASED;
static unsigned clicks;

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    (void)area; (void)pixels; lv_display_flush_ready(display);
}
static void read_input(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev; data->key = LV_KEY_ENTER; data->state = physical_state;
}
static void clicked(lv_event_t *event) { (void)event; ++clicks; }
static void physical(lv_indev_state_t state) {
    physical_state = state; lv_tick_inc(20); lv_indev_read(input);
}
static void click(void) {
    physical(LV_INDEV_STATE_PRESSED); physical(LV_INDEV_STATE_RELEASED);
}
static void resume(bool read_current) {
    lv_indev_wait_release(input); lv_indev_enable(input, true);
    if (read_current) lv_indev_read(input);
}

int main(void) {
    lv_init(); lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof buffer, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_group_t *group = lv_group_create();
    lv_obj_t *button = lv_button_create(lv_screen_active());
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_group_add_obj(group, button);
    lv_obj_add_event_cb(button, clicked, LV_EVENT_CLICKED, NULL);
    input = lv_indev_create(); lv_indev_set_type(input, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_mode(input, LV_INDEV_MODE_EVENT);
    lv_indev_set_read_cb(input, read_input); lv_indev_set_group(input, group);
    lv_indev_set_disp(input, display);
    click(); assert(clicks == 1);

    /* Characterize the regression with the actual event-mode encoder: its
     * disabled RELEASE is not read, and enable alone schedules no fresh read. */
    physical(LV_INDEV_STATE_PRESSED); lv_indev_enable(input, false);
    physical(LV_INDEV_STATE_RELEASED); resume(false);
    lv_tick_inc(100); lv_timer_handler();
    click(); assert(clicks == 1); // The first new gesture was incorrectly lost.
    click(); assert(clicks == 2);

    /* Already released: consuming the current state must produce no old click,
     * and the very next new press/release must work. */
    physical(LV_INDEV_STATE_PRESSED); lv_indev_enable(input, false);
    physical(LV_INDEV_STATE_RELEASED); resume(true);
    assert(clicks == 2);
    click(); assert(clicks == 3);

    /* Still held: the synchronous read must keep waiting for its release,
     * consume that tail, then accept the next complete gesture. */
    physical(LV_INDEV_STATE_PRESSED); lv_indev_enable(input, false);
    resume(true); assert(clicks == 3);
    physical(LV_INDEV_STATE_RELEASED); assert(clicks == 3);
    click(); assert(clicks == 4);
    assert(lv_mem_test() == LV_RESULT_OK);
    puts("Navigation resume PASS: event-mode regression reproduced; old release consumed and next gesture accepted");
}
