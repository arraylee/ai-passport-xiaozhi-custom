/* Actual LVGL paths: the full journey below uses only short up/down/OK.
 * Long back is exercised separately as a compatibility shortcut. */
#include "lvgl.h"
#include "presentation/ui_bobo.h"
#include "presentation/bobo_art.h"
#include "services/bobo_content.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned commands, argument, exits;
static ui_bobo_command_t command;
static bool accept = true;
static bobo_state_t state;
static uint16_t buffer[240 * 20];
static uint16_t framebuffer[240 * 320];
static unsigned screenshot_index;
static bool enqueue(ui_bobo_command_t c, unsigned a, void *user) {
    (void)user;
    if (!accept) return false;
    commands++; command = c; argument = a; return true;
}
static void on_exit(void *user) { (void)user; exits++; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *source = (const uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) framebuffer[y * 240 + x] = *source++;
    lv_display_flush_ready(display);
}
static bool contains(lv_obj_t *object, const char *needle, bool focused) {
    if (lv_obj_check_type(object, &lv_label_class) && strstr(lv_label_get_text(object), needle) &&
        (!focused || lv_obj_get_style_border_width(lv_obj_get_parent(object), 0) == 2)) return true;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        if (contains(lv_obj_get_child(object, i), needle, focused)) return true;
    return false;
}
static void check_label_bounds(lv_obj_t *object) {
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t label, parent;
        lv_obj_get_coords(object, &label);
        lv_obj_get_coords(lv_obj_get_parent(object), &parent);
        if (label.x1 < parent.x1 || label.x2 > parent.x2 || label.y1 < parent.y1 || label.y2 > parent.y2)
            fprintf(stderr, "Clipped label: %s\n", lv_label_get_text(object));
        assert(label.x1 >= parent.x1 && label.x2 <= parent.x2 && label.y1 >= parent.y1 && label.y2 <= parent.y2);
        lv_color_t text_color = lv_obj_get_style_text_color(object, 0);
        /* Surfaces never double as text tokens on the dark theme. */
        assert(lv_color_eq(text_color, lv_color_hex(0xF2F1E8)) ||
               lv_color_eq(text_color, lv_color_hex(0xAEC4CD)) ||
               lv_color_eq(text_color, lv_color_hex(0xA8D6C4)) ||
               lv_color_eq(text_color, lv_color_hex(0xE5B8A0)) ||
               lv_color_eq(text_color, lv_color_hex(0xE8D2A0)));
        lv_obj_t *container = lv_obj_get_parent(object);
        if (lv_obj_get_style_border_width(container, 0) == 2) {
            assert(lv_color_eq(lv_obj_get_style_bg_color(container, 0), lv_color_hex(0x365766)));
            assert(lv_color_eq(lv_obj_get_style_text_color(object, 0), lv_color_hex(0xF2F1E8)));
            assert(lv_color_eq(lv_obj_get_style_border_color(container, 0), lv_color_hex(0xA8D6C4)));
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        check_label_bounds(lv_obj_get_child(object, i));
}
static lv_obj_t *find_image(lv_obj_t *object, const lv_image_dsc_t *source) {
    if (lv_obj_check_type(object, &lv_image_class) && lv_image_get_src(object) == source) return object;
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) {
        lv_obj_t *found = find_image(lv_obj_get_child(object, i), source);
        if (found) return found;
    }
    return NULL;
}
static void capture_frame(const char *directory, unsigned index, bool motion) {
    if (!directory) return;
    char path[1024];
    snprintf(path, sizeof(path), motion ? "%s/motion-%04u.ppm" : "%s/bobo-%02u.ppm", directory, index);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n240 320\n255\n");
    for (unsigned i = 0; i < 240 * 320; ++i) {
        uint16_t color = framebuffer[i];
        uint8_t rgb[] = {((color >> 11) & 31) * 255 / 31, ((color >> 5) & 63) * 255 / 63, (color & 31) * 255 / 31};
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
}
static void capture_optional(void) {
    capture_frame(getenv("BOBO_UI_SCREENSHOT_DIR"), ++screenshot_index, false);
}
static void screen_has(const char *needle) {
    lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
    assert(lv_color_eq(lv_obj_get_style_bg_color(lv_screen_active(), 0), lv_color_hex(0x1B3441)));
    if (!contains(lv_screen_active(), needle, false)) fprintf(stderr, "Missing screen text: %s (active pet %u, command %u)\n", needle, state.active_pet, command);
    assert(contains(lv_screen_active(), needle, false));
    assert(contains(lv_screen_active(), "返回", false));
    check_label_bounds(lv_screen_active());
    capture_optional();
    assert(lv_mem_test() == LV_RESULT_OK);
}
static void ok(void) { ui_bobo_key(UI_PLAY_OK); }
static void down(void) { ui_bobo_key(UI_PLAY_DOWN); }
static void choose(const char *label) {
    for (unsigned n = 0; n < 7; ++n) {
        if (contains(lv_screen_active(), label, true)) { ok(); return; }
        down();
    }
    fprintf(stderr, "Could not focus: %s\n", label); assert(false);
}
static void back(void) { choose("返回"); }
static void open_state(bobo_load_result_t load) {
    ui_bobo_open(&state, load, true, enqueue, on_exit, NULL);
}
static void ack(bool save) {
    bobo_result_t result = BOBO_OK;
    if (save) switch (command) {
        case UI_BOBO_CARE: result = bobo_care(&state, (bobo_care_t)argument); break;
        case UI_BOBO_EQUIP: result = bobo_equip(&state, (bobo_tool_t)argument); break;
        case UI_BOBO_TARGET: result = bobo_target(&state, argument); break;
        case UI_BOBO_START: result = bobo_start(&state, argument); break;
        case UI_BOBO_CANCEL: result = bobo_cancel(&state); break;
        case UI_BOBO_CLAIM: result = bobo_claim(&state); break;
        case UI_BOBO_SELECT_PET: result = bobo_pet_select(&state, argument); break;
        case UI_BOBO_REVEAL_ACK: result = bobo_pet_reveal_ack(&state); break;
        case UI_BOBO_WISH_START: case UI_BOBO_WISH_CANCEL: case UI_BOBO_RETRY_SAVE: break;
    }
    ui_bobo_update(&state, result, save, true, true);
}
static void prepare_trip(void) {
    choose("旅行"); choose("纹样小馆"); choose("准备行囊"); choose("轻装出发");
    screen_has("确认出发"); assert(state.phase == BOBO_HOME);
}
static void animate_frames(unsigned count) {
    for (unsigned i = 0; i < count; ++i) { lv_tick_inc(160); lv_timer_handler(); }
    lv_obj_update_layout(lv_screen_active()); lv_refr_now(NULL);
    assert(lv_mem_test() == LV_RESULT_OK);
}
static unsigned timer_count(void) {
    unsigned count = 0;
    for (lv_timer_t *timer = lv_timer_get_next(NULL); timer; timer = lv_timer_get_next(timer)) ++count;
    return count;
}
static lv_obj_t *home_pet(void) {
    static const lv_image_dsc_t *const images[3][4] = {
        {&bobo_baby_home_idle, &bobo_baby_home_wave, &bobo_baby_home_happy, &bobo_baby_home_sleep},
        {&bobo_junior_home_idle, &bobo_junior_home_wave, &bobo_junior_home_happy, &bobo_junior_home_sleep},
        {&bobo_grown_home_idle, &bobo_grown_home_wave, &bobo_grown_home_happy, &bobo_grown_home_sleep}
    };
    for (unsigned i = 0; i < 4; ++i) {
        lv_obj_t *image = find_image(lv_screen_active(), images[bobo_growth_stage(&state)][i]);
        if (image) return image;
    }
    assert(false); return NULL;
}
static void check_ambient_animation(void) {
    bobo_init(&state); state.owned_pets = 1;
    unsigned timers = timer_count(), initial_commands = commands;
    open_state(BOBO_SAVE_LOADED); assert(timer_count() == timers + 1);
    const char *motion_directory = getenv("BOBO_UI_MOTION_DIR");
    animate_frames(0); capture_frame(motion_directory, 0, true);
    for (unsigned i = 0; i < 16; ++i) { animate_frames(1); capture_frame(motion_directory, i + 1, true); }
    unsigned poses = 0, moving_frames = 0;
    bobo_state_t saved = state;
    for (unsigned i = 0; i < 600; ++i) {
        animate_frames(1);
        if (i < 160) capture_frame(motion_directory, i + 17, true);
        lv_obj_t *hero = home_pet();
        const void *source = lv_image_get_src(hero);
        int x = lv_obj_get_x(hero), y = lv_obj_get_y(hero);
        assert(!lv_obj_has_flag(hero, LV_OBJ_FLAG_HIDDEN));
        assert(x >= 56 && x <= 64 && y >= 69 && y <= 79);
        if (x != 60 || y != 78) ++moving_frames;
        if (source == &bobo_baby_home_idle) poses |= 1;
        if (source == &bobo_baby_home_wave) poses |= 2;
        if (source == &bobo_baby_home_happy) poses |= 4;
        if (source == &bobo_baby_home_sleep) poses |= 8;
        /* Worker clock refreshes and focus moves retain the current frame. */
        if (i % 6 == 0) ui_bobo_update(&state, BOBO_OK, true, false, true);
        if (i % 30 == 0 && !motion_directory) down(); // export shows undisturbed ambient motion
        lv_obj_update_layout(lv_screen_active()); hero = home_pet();
        assert(lv_image_get_src(hero) == source);
        assert(lv_obj_get_x(hero) == x && lv_obj_get_y(hero) == y);
    }
    assert(poses == 15 && moving_frames > 100);
    assert(commands == initial_commands && !memcmp(&saved, &state, sizeof(state)));
    /* Pause resets the image to a harmless baseline, and every sleeping input
     * is ignored. A background worker refresh cannot resume a paused ticker. */
    ui_bobo_suspend(true);
    lv_obj_update_layout(lv_screen_active());
    lv_obj_t *hero = home_pet(); const void *source = lv_image_get_src(hero);
    int x = lv_obj_get_x(hero), y = lv_obj_get_y(hero);
    ui_bobo_key(UI_PLAY_OK); ui_bobo_key(UI_PLAY_BACK);
    ui_bobo_update(&state, BOBO_OK, true, false, true);
    animate_frames(100); hero = home_pet();
    assert(lv_image_get_src(hero) == source && lv_obj_get_x(hero) == x && lv_obj_get_y(hero) == y);
    ui_bobo_suspend(false); animate_frames(30);
    choose("照顾"); screen_has("陪博博待一会儿");
    unsigned children = lv_obj_get_child_count(lv_screen_active());
    animate_frames(100); assert(lv_obj_get_child_count(lv_screen_active()) == children);
    screen_has("陪博博待一会儿");
    back(); assert(!lv_obj_has_flag(home_pet(), LV_OBJ_FLAG_HIDDEN));
    back(); animate_frames(30); assert(timer_count() == timers);
    /* Low energy has breathing/resting/looking beats but no energetic hop. */
    bobo_init(&state); state.owned_pets = 1; state.energy = 20; open_state(BOBO_SAVE_LOADED); animate_frames(16);
    bool rested = false;
    for (unsigned i = 0; i < 400; ++i) {
        animate_frames(1); hero = home_pet();
        assert(lv_image_get_src(hero) != &bobo_baby_home_happy);
        assert(lv_obj_get_y(hero) >= 77);
        if (lv_image_get_src(hero) == &bobo_baby_home_sleep) rested = true;
    }
    assert(rested); back(); assert(timer_count() == timers);
    /* A travelling pet walks/looks/waves, while a new discovery can celebrate.
     * These visual beats do not advance or claim the actual trip. */
    bobo_init(&state); state.owned_pets = 1; assert(bobo_start(&state, 0) == BOBO_OK);
    open_state(BOBO_SAVE_LOADED); animate_frames(16); saved = state;
    for (unsigned i = 0; i < 240; ++i) {
        animate_frames(1); hero = home_pet();
        assert(lv_image_get_src(hero) != &bobo_baby_home_sleep);
        assert(lv_obj_get_y(hero) >= 76);
    }
    assert(!memcmp(&saved, &state, sizeof(state))); back();
    bobo_advance(&state, 180); assert(state.phase == BOBO_READY);
    open_state(BOBO_SAVE_LOADED); animate_frames(16); saved = state;
    bool celebrated = false;
    for (unsigned i = 0; i < 240; ++i) {
        animate_frames(1);
        if (lv_image_get_src(home_pet()) == &bobo_baby_home_happy && lv_obj_get_y(home_pet()) < 76) celebrated = true;
    }
    assert(celebrated && !memcmp(&saved, &state, sizeof(state)));
    back(); assert(timer_count() == timers && commands == initial_commands);
}
static void start_wish(void) {
    choose("开始许愿"); assert(command == UI_BOBO_WISH_START); ack(true);
    screen_has("先安静片刻"); screen_has("正在听周围的声音");
}
static void check_pet_gifts(void) {
    unsigned timers = timer_count();
    bobo_init(&state);
    ui_bobo_open(&state, BOBO_SAVE_NEW, false, enqueue, on_exit, NULL);
    screen_has("许愿小礼盒"); assert(!contains(lv_screen_active(), "校准时间", false)); back();
    bobo_init(&state); open_state(BOBO_SAVE_NEW);
    screen_has("许愿小礼盒"); assert(!contains(lv_screen_active(), "宝宝熊", false));
    start_wish();
    pet_wish_view_t view = {.phase = PET_WISH_LISTENING, .progress = 500};
    ui_bobo_wish_update(&view, true); screen_has("现在说出你的愿望"); screen_has("慢慢说");
    assert(find_image(lv_screen_active(), &pet_gift_2));
    animate_frames(100); assert(find_image(lv_screen_active(), &pet_gift_2));
    assert(!state.owned_pets && !state.pending_reveal);
    back(); assert(command == UI_BOBO_WISH_CANCEL); ack(true); assert(!ui_bobo_is_active());
    ui_bobo_wish_update(&view, true); assert(!ui_bobo_is_active()); // Late audio cannot reopen the UI.

    open_state(BOBO_SAVE_NEW); start_wish();
    view.phase = PET_WISH_TIMED_OUT; ui_bobo_wish_update(&view, true); screen_has("再说一次");
    choose("再说一次"); ack(true);
    view.phase = PET_WISH_MIC_ERROR; ui_bobo_wish_update(&view, true); screen_has("麦克风暂时不可用");
    back(); assert(!ui_bobo_is_active());

    open_state(BOBO_SAVE_NEW); start_wish();
    view.phase = PET_WISH_ACCEPTED; view.progress = 1000;
    ui_bobo_wish_update(&view, true); // Sound acceptance without a committed acquisition is insufficient.
    assert(!find_image(lv_screen_active(), &pet_panda_baby_home_idle));
    assert(!find_image(lv_screen_active(), &bobo_baby_home_idle));
    screen_has("再说一次"); back();

    open_state(BOBO_SAVE_NEW); start_wish();
    bobo_state_t candidate = state;
    assert(bobo_pet_draw(&candidate, 0xB3333333u) == BOBO_OK);
    ui_bobo_wish_update(&view, false); // Candidate has not been published after failed storage.
    screen_has("礼物还没保存好");
    assert(!contains(lv_screen_active(), "熊猫", false));
    assert(!find_image(lv_screen_active(), &pet_panda_baby_home_idle));
    assert(!state.owned_pets); back(); assert(!ui_bobo_is_active());

    open_state(BOBO_SAVE_NEW); start_wish();
    state = candidate; // The real worker publishes only after journal commit.
    ui_bobo_update(&state, BOBO_OK, true, false, true);
    assert(!find_image(lv_screen_active(), &pet_panda_baby_home_idle));
    ui_bobo_wish_update(&view, true); screen_has("礼盒正在打开");
    unsigned count = commands; ok(); assert(commands == count && state.pending_reveal);
    animate_frames(10); back(); assert(!ui_bobo_is_active() && state.pending_reveal);
    unsigned sequence = state.draw_sequence;
    open_state(BOBO_SAVE_LOADED); screen_has("礼盒正在打开");
    assert(commands == count && state.draw_sequence == sequence);
    ui_bobo_suspend(true); animate_frames(30); ok(); assert(commands == count);
    ui_bobo_suspend(false); animate_frames(20); screen_has("带它回家"); screen_has("熊猫，来啦");
    choose("带它回家"); assert(command == UI_BOBO_REVEAL_ACK);
    ack(false); screen_has("还没保存好"); assert(state.pending_reveal);
    choose("重试存档"); ack(true); back(); screen_has("礼盒正在打开");
    animate_frames(20); choose("带它回家"); ack(true);
    screen_has("熊猫的小屋"); screen_has("再领取 3 次旅行");
    assert(!state.pending_reveal && state.draw_sequence == sequence);
    choose("伙伴"); screen_has("我的小伙伴"); screen_has("熊猫");
    assert(!contains(lv_screen_active(), "向日葵", false));
    assert(!contains(lv_screen_active(), "许愿", false));
    count = commands; down(); ok(); assert(commands == count); // The only action is return.
    screen_has("熊猫的小屋"); back();
    // Migrated collection is preserved without exposing selectable species.
    state.owned_pets = BOBO_PET_MASK; state.life_journeys = 3;
    open_state(BOBO_SAVE_LOADED); choose("伙伴"); screen_has("少年熊猫");
    assert(!contains(lv_screen_active(), "向日葵", false)); back();
    bobo_advance(&state, BOBO_ILL_AFTER_S);
    ui_bobo_update(&state, BOBO_OK, true, false, false);
    screen_has("好好休息");
    assert(find_image(lv_screen_active(), &pet_panda_junior_home_sleep));
    choose("照顾"); choose("喝水"); ok(); ack(true); back(); back(); back();
    assert(state.health == BOBO_WELL && !state.neglect_s);
    bobo_advance(&state, BOBO_DEATH_AFTER_S);
    ui_bobo_update(&state, BOBO_OK, true, false, false);
    screen_has("和伙伴说再见"); screen_has("再次许愿");
    assert(find_image(lv_screen_active(), &pet_panda_junior_home_sleep));
    count = commands; back(); assert(!ui_bobo_is_active() && commands == count);
    open_state(BOBO_SAVE_LOADED); screen_has("和伙伴说再见");
    choose("再次许愿"); screen_has("许愿小礼盒"); start_wish();
    count = commands; ui_bobo_close();
    assert(commands == count + 1 && command == UI_BOBO_WISH_CANCEL);
    assert(timer_count() == timers);

    // A missing mapped pack is a readable return page, never empty image descriptors.
    bobo_art_unbind(); open_state(BOBO_SAVE_LOADED);
    screen_has("小伙伴还在准备");
    assert(!find_image(lv_screen_active(), &pet_gift_0));
    back(); assert(!ui_bobo_is_active() && timer_count() == timers);
}
int main(void) {
    lv_init(); lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer, NULL, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_t *previous = lv_screen_active();
    /* The three native age sprites keep their real proportions and stand on
     * the same room baseline; no runtime rescaling hides the growth change. */
    const unsigned counts[] = {0, 3, 8};
    const int sprite_y[] = {78, 70, 64};
    const lv_image_dsc_t *const stages[] = {&bobo_baby_home_idle, &bobo_junior_home_idle, &bobo_grown_home_idle};
    for (unsigned i = 0; i < 3; ++i) {
        bobo_init(&state); state.owned_pets = 1; state.collected = (1u << counts[i]) - 1; state.life_journeys = counts[i];
        open_state(BOBO_SAVE_LOADED); screen_has(bobo_growth_name(&state));
        lv_obj_t *hero = find_image(lv_screen_active(), stages[i]); assert(hero);
        /* There is a real anticipation before the companion arrives. */
        assert(lv_obj_has_flag(hero, LV_OBJ_FLAG_HIDDEN));
        animate_frames(4); assert(lv_obj_has_flag(hero, LV_OBJ_FLAG_HIDDEN));
        animate_frames(12);
        assert(!lv_obj_has_flag(hero, LV_OBJ_FLAG_HIDDEN));
        lv_area_t position; lv_obj_get_coords(hero, &position);
        assert(position.x1 == 60 && position.y1 == sprite_y[i]);
        assert(lv_obj_get_width(hero) == 120 && lv_obj_get_height(hero) == 120);
        back();
    }
    bobo_init(&state); open_state(BOBO_SAVE_NEW);
    screen_has("许愿小礼盒"); back(); assert(!ui_bobo_is_active());
    check_ambient_animation();
    bobo_init(&state); state.owned_pets = 1; open_state(BOBO_SAVE_LOADED);
    assert(contains(lv_screen_active(), "照顾", false) && contains(lv_screen_active(), "背包", false) &&
           contains(lv_screen_active(), "旅行", false) && contains(lv_screen_active(), "图鉴", false) &&
           contains(lv_screen_active(), "返回", false));
    /* HOME fifth item exits without knowing the long-press shortcut. */
    ui_bobo_key(UI_PLAY_UP); screen_has("返回"); ok();
    assert(!ui_bobo_is_active() && lv_screen_active() == previous);
    open_state(BOBO_SAVE_LOADED); choose("照顾"); choose("喝水"); screen_has("精神 +15");
    back(); screen_has("陪博博待一会儿"); choose("喝水"); ok();
    assert(command == UI_BOBO_CARE && commands == 1);
    ok(); down(); ui_bobo_key(UI_PLAY_BACK); assert(commands == 1);
    ui_bobo_update(&state, BOBO_OK, true, false, true); ok(); assert(commands == 1);
    ack(true); screen_has("精神 +15"); back(); back(); back();
    choose("背包"); screen_has("博博的小背包");
    static const char *const items[] = {"水壶", "小毛巾", "点心", "速写本", "观察镜"};
    for (unsigned i = 0; i < 5; ++i) { choose(items[i]); screen_has(items[i]); back(); }
    choose("观察镜"); choose("下次带上"); assert(command == UI_BOBO_EQUIP && argument == BOBO_LENS);
    ack(true); back(); back(); back();
    choose("旅行"); choose("造物小馆"); screen_has("还未解锁"); back();
    choose("纹样小馆"); screen_has("准备行囊"); back(); choose("纹样小馆"); choose("准备行囊");
    screen_has("这次带什么"); back(); choose("准备行囊"); choose("速写本");
    assert(command == UI_BOBO_EQUIP && argument == BOBO_SKETCHBOOK);
    unsigned count = commands; ok(); assert(commands == count); ack(true);
    screen_has("确认出发"); back(); back(); back(); back(); assert(state.phase == BOBO_HOME);
    choose("旅行"); choose("纹样小馆"); choose("准备行囊"); choose("速写本");
    choose("确认出发"); assert(command == UI_BOBO_START); ack(true); screen_has("在路上");
    back(); assert(state.phase == BOBO_TRAVELLING); choose("旅行");
    choose("提前回来"); screen_has("现在就回来吗");
    assert(contains(lv_screen_active(), "返回", true)); ok(); screen_has("在路上");
    choose("提前回来"); choose("确认提前回来"); assert(command == UI_BOBO_CANCEL); ack(false);
    screen_has("还没保存好"); assert(state.phase == BOBO_TRAVELLING);
    accept = false; choose("重试存档"); screen_has("请再试一次"); accept = true;
    choose("重试存档"); assert(command == UI_BOBO_RETRY_SAVE); ack(true); back(); screen_has("在路上");
    choose("提前回来");
    choose("确认提前回来"); ack(true); back();
    /* READY short return never claims; reward uses current actual capacity. */
    bobo_init(&state); state.owned_pets = 1; assert(bobo_start(&state, 0) == BOBO_OK); bobo_advance(&state, 180);
    state.snacks = 9; ui_bobo_update(&state, BOBO_OK, true, false, true);
    choose("旅行"); screen_has("点心实际入包 0 份"); back(); assert(state.phase == BOBO_READY);
    choose("旅行"); choose("收进观察册"); assert(command == UI_BOBO_CLAIM); ack(true);
    screen_has("点心 +0"); back(); choose("图鉴"); screen_has("博博的观察册");
    back(); choose("图鉴"); choose("纹样小馆"); screen_has("已得  波浪线"); back();
    choose("纹样小馆"); choose("波浪线"); screen_has("观察发现 1/2");
    choose("下一页"); screen_has("博博想把小浪花带回家"); choose("下一页"); screen_has("观察发现 1/2");
    back(); choose("螺旋线"); screen_has("设为当前目标"); back(); choose("螺旋线"); choose("设为当前目标");
    assert(command == UI_BOBO_TARGET && argument == 1); ack(true); back(); screen_has("取消目标");
    back(); back(); back(); back(); assert(!ui_bobo_is_active() && lv_screen_active() == previous);
    unsigned closed = exits; ui_bobo_update(&state, BOBO_OK, true, true, false); assert(exits == closed);
    /* Unavailable and failed-save pages have an immediate short exit too. */
    open_state(BOBO_SAVE_UNAVAILABLE); back(); assert(!ui_bobo_is_active());
    open_state(BOBO_SAVE_UNAVAILABLE); choose("重试存档"); ack(true); back(); back(); assert(!ui_bobo_is_active());
    bobo_init(&state); state.owned_pets = 1; open_state(BOBO_SAVE_LOADED); choose("照顾"); choose("喝水"); ok(); ack(false);
    back(); assert(!ui_bobo_is_active() && state.energy == 70);
    open_state(BOBO_SAVE_LOADED); accept = false; choose("照顾"); choose("喝水"); ok();
    screen_has("请再试一次"); back(); screen_has("喝水"); accept = true; back(); back(); back();
    open_state(BOBO_SAVE_RECOVERED); screen_has("恢复上一次"); back(); back();
    open_state(BOBO_SAVE_RESET); screen_has("新的小旅行"); back(); back();
    /* No-card receipt and locked-detail single-return branches. */
    bobo_init(&state); state.owned_pets = 1; state.collected = 0xfff; state.intro_used = 1; open_state(BOBO_SAVE_LOADED);
    prepare_trip(); choose("确认出发"); ack(true); bobo_advance(&state, 600);
    ui_bobo_update(&state, BOBO_OK, true, false, true); choose("收进观察册"); ack(true);
    screen_has("12/12"); back(); back();
    /* Check every paper-card observation and memory at the actual font size. */
    open_state(BOBO_SAVE_LOADED); choose("图鉴");
    for (unsigned book = 0; book < BOBO_DESTINATION_COUNT; ++book) {
        choose(bobo_destinations[book]);
        for (unsigned card = book * 4; card < book * 4 + 4; ++card) {
            choose(bobo_cards[card].name); screen_has("观察发现 1/2");
            choose("下一页"); screen_has("博博的回忆 2/2"); back();
        }
        back();
    }
    back(); back();
    /* An uncertain IO result may already be committed. Re-read must route to
     * the loaded phase/receipt instead of re-offering the previous command. */
    bobo_init(&state); state.owned_pets = 1; assert(bobo_start(&state, 0) == BOBO_OK); bobo_advance(&state, 180);
    open_state(BOBO_SAVE_LOADED); choose("旅行"); choose("收进观察册"); ack(false);
    assert(bobo_claim(&state) == BOBO_OK); choose("重试存档"); ack(true); back();
    screen_has("这次发现收好啦"); assert(!contains(lv_screen_active(), "收进观察册", false));
    back(); back();
    bobo_init(&state); state.owned_pets = 1; open_state(BOBO_SAVE_LOADED); prepare_trip(); choose("确认出发"); ack(false);
    assert(bobo_start(&state, 0) == BOBO_OK); choose("重试存档"); ack(true); back();
    screen_has("在路上"); assert(!contains(lv_screen_active(), "确认出发", false));
    choose("提前回来"); choose("确认提前回来"); ack(false);
    assert(bobo_cancel(&state) == BOBO_OK); choose("重试存档"); ack(true); back();
    screen_has("宝宝熊"); assert(!contains(lv_screen_active(), "确认提前回来", false)); back();
    /* The new unique-card thresholds change the real age-specific image and
     * show a celebration only after a saved claim, then a valid receipt. */
    for (unsigned threshold = 3; threshold <= 8; threshold += 5) {
        bobo_init(&state); state.owned_pets = 1; state.collected = (1u << (threshold - 1)) - 1; state.life_journeys = threshold - 1;
        assert(bobo_start(&state, threshold == 3 ? 0 : 1) == BOBO_OK); bobo_advance(&state, 180);
        open_state(BOBO_SAVE_LOADED); choose("旅行"); choose("收进观察册");
        ack(true); screen_has("博博长大啦");
        screen_has(threshold == 3 ? "少年熊" : "大熊");
        choose("看看这次收获"); screen_has("这次发现收好啦"); back(); back();
    }
    /* Long back remains the same shortcut, tested independently. */
    bobo_init(&state); state.owned_pets = 1; open_state(BOBO_SAVE_LOADED); choose("照顾"); choose("喝水");
    ui_bobo_key(UI_PLAY_BACK); screen_has("陪博博待一会儿");
    ui_bobo_key(UI_PLAY_BACK); ui_bobo_key(UI_PLAY_BACK); assert(!ui_bobo_is_active());
    for (unsigned i = 0; i < 100; ++i) {
        open_state(BOBO_SAVE_LOADED); choose("背包"); choose("观察镜"); back(); back(); back();
    }
    check_pet_gifts();
    assert(lv_mem_test() == LV_RESULT_OK);
    puts("Bobo UI PASS: three-stage summon, random native frames, low-energy rest, sleep/refresh/navigation cancellation, timer cleanup, all pages short-return, save/retry and full journey, 100 reopen cycles.");
    return 0;
}
