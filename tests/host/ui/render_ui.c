#include "presentation/ui_xiaozhi.h"
#include "presentation/ui_boot.h"
#include "lvgl.h"
#include "presentation/ui_profile.h"
#include "presentation/ui_album.h"
#include "presentation/ui_theme.h"
#include "presentation/bobo_art.h"
#include "presentation/ui_plays.h"
#include "presentation/ui_game.h"
#include "presentation/ui_button_guide.h"
#include "presentation/ui_selftest.h"
#include "presentation/ui_licenses.h"
#include "presentation/passport_font_blocks.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef PASSPORT_PLATFORM_DISPLAY
#include "platform_display_native.h"
#endif
LV_FONT_DECLARE(lv_font_cn_16);

/* Preview input only: keep the real profile clock UI deterministic across
 * separate compressed/plain renderer processes and minute boundaries. */
time_t time(time_t *result) {
    const time_t preview_time = 1789382520; /* 2026-09-14 10:42 UTC */
    if (result) *result = preview_time;
    return preview_time;
}

static uint16_t framebuffer[240 * 320];
static uint16_t draw_buffer[240 * 20];
static const char *output_dir;
/* Match png_decode.c's actual BGRA byte order and ARGB8888 descriptor. */
static uint8_t avatar_pixels[96 * 156 * 4];
static lv_image_dsc_t avatar = {.header = {.magic=LV_IMAGE_HEADER_MAGIC, .cf=LV_COLOR_FORMAT_ARGB8888,
    .w=96, .h=156, .stride=384}, .data_size=sizeof(avatar_pixels), .data=avatar_pixels};
static uint8_t companion_pixels[2][6][144 * 144 * 3];
static lv_image_dsc_t companion_frames[2][6];
static companion_portrait_t companion_images[2];
static const companion_portrait_t *voice_art(unsigned index) {
    const char *path = getenv("COMPANION_PACK");
    if (!path) return NULL;
    if (!companion_images[0].frames) {
        FILE *file = fopen(path, "rb"); assert(file);
        uint32_t header[32]; assert(fread(header, 1, sizeof header, file) == sizeof header);
        assert(!memcmp(header, "CPR1", 4) && (header[1]==2 || header[1]==3));
        for (unsigned i = 0; i < 2; i++) {
            companion_images[i].frames=companion_frames[i];
            for(unsigned frame=0;frame<6;frame++) {
                unsigned entry=2+i*6+frame;
                assert(header[5+entry*2] == sizeof companion_pixels[i][frame]);
                assert(fseek(file, header[4+entry*2], SEEK_SET) == 0);
                assert(fread(companion_pixels[i][frame], 1, sizeof companion_pixels[i][frame], file) == sizeof companion_pixels[i][frame]);
                companion_frames[i][frame] = (lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,
                    .cf=LV_COLOR_FORMAT_RGB565A8, .w=144, .h=144, .stride=288},
                    .data_size=sizeof companion_pixels[i][frame], .data=companion_pixels[i][frame]};
            }
        }
        fclose(file);
    }
    return &companion_images[index];
}

static void load_avatar(const char *path) {
    FILE *source = fopen(path, "rb"); assert(source);
    assert(fread(avatar_pixels, 1, sizeof(avatar_pixels), source) == sizeof(avatar_pixels));
    fclose(source);
}

static void check_glyphs(lv_obj_t *object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        const unsigned char *p = (const unsigned char *)lv_label_get_text(object);
        const lv_font_t *font = lv_obj_get_style_text_font(object, 0);
        while (*p) {
            uint32_t c = *p++;
            if (c >= 0xc0) {
                unsigned count = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
                c &= count == 1 ? 31 : count == 2 ? 15 : 7;
                while (count--) c = (c << 6) | (*p++ & 63);
            }
            if (c < 32) continue;
            lv_font_glyph_dsc_t glyph = {0};
            if (!lv_font_get_glyph_dsc(font, &glyph, c, 0) || glyph.is_placeholder) {
                fprintf(stderr, "Missing glyph U+%04X in %s\n", c, lv_label_get_text(object)); abort();
            }
            if (c >= 0x2e80 && c <= 0x9fff) {
                const lv_font_t *resolved = glyph.resolved_font;
                assert(resolved == &ui_font_body || resolved == &ui_font_title ||
                       resolved == &ui_font_caption || resolved == &ui_font_answer || resolved == &ui_font_book ||
                       resolved == &lv_font_cn_16 || resolved->get_glyph_bitmap == passport_font_get_bitmap);
                assert(((const lv_font_fmt_txt_dsc_t *)resolved->dsc)->bpp == 4);
            }
        }
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i) check_glyphs(lv_obj_get_child(object, i));
}
static void game_key(ui_game_key_t key) {
    ui_game_key(key); lv_tick_inc(40); lv_timer_handler();
}

static void check_label_bounds(lv_obj_t *object) {
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(object, &lv_label_class)) {
        lv_area_t area; lv_obj_get_coords(object, &area);
        assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < 240 && area.y2 < 320);
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(object); ++i)
        check_label_bounds(lv_obj_get_child(object, i));
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint16_t *source = (uint16_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) {
            assert(x >= 0 && x < 240 && y >= 0 && y < 320);
            framebuffer[y * 240 + x] = *source++;
        }
    lv_display_flush_ready(display);
}

static void screenshot(const char *name) {
#ifdef PASSPORT_PLATFORM_DISPLAY
    printf("RENDER_BEGIN %s\n", name); fflush(stdout);
#endif
    lv_tick_inc(40); lv_timer_handler(); lv_refr_now(NULL);
    check_glyphs(lv_screen_active());
    check_label_bounds(lv_screen_active());
    char path[1024]; snprintf(path, sizeof(path), "%s/%s.ppm", output_dir, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n240 320\n255\n");
    for (unsigned i = 0; i < 240 * 320; ++i) {
        uint16_t c = framebuffer[i];
        uint8_t rgb[] = {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
        fwrite(rgb, 1, sizeof(rgb), file);
    }
    fclose(file);
    lv_mem_monitor_t memory; lv_mem_monitor(&memory);
    printf("%s: current=%zu peak=%zu pool=%zu largest=%zu\n", name,
        memory.total_size - memory.free_size, memory.max_used, memory.total_size, memory.free_biggest_size);
    assert(lv_mem_test() == LV_RESULT_OK);
}

static void render_selftest(lv_obj_t *profile) {
    ui_selftest_open("PREVIEW01");
    const char *items[] = {"INFO", "I2C", "AUDIO", "BATT", "BLE", "ID", "DISP", "BTN"};
    for (unsigned i = 0; i < 8; ++i) ui_selftest_set_item(items[i], UI_ST_PASS);
    ui_selftest_battery(70);
    ui_selftest_prompt("各按两下: 上 0/2  下 0/2  确定 0/2");
    screenshot("selftest-progress"); check_label_bounds(lv_screen_active());
    ui_selftest_result(true); screenshot("selftest-result"); check_label_bounds(lv_screen_active());
    lv_screen_load(profile); ui_selftest_close();
}

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4); output_dir = argv[1];
    load_avatar(argv[2]);
    lv_init();
#ifdef PASSPORT_PLATFORM_DISPLAY
    lv_display_t *display = native_platform_display(flush);
    (void)display;
#else
    lv_display_t *display = lv_display_create(240, 320);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, draw_buffer, NULL, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
#endif
    bool factory_boot = getenv("SELFTEST_PREVIEW_ONLY") != NULL;
    // app_main skips navigation and saved-avatar restoration in factory boot.
    if (!factory_boot) lv_group_set_default(lv_group_create());
    if (factory_boot) {
        render_selftest(lv_screen_active());
        lv_obj_t *empty = ui_theme_screen(); lv_screen_load(empty);
        screenshot("theme-rounded-reference");
        lv_obj_set_style_bg_color(empty, lv_color_hex(0x0B1118), 0);
        screenshot("xiaozhi-rounded-reference");
        return 0;
    }
    ui_profile_create();
    ui_profile_set_name("FoloToy");
    ui_profile_set_title("NETRUNNER");
    ui_profile_set_intro("");
    ui_profile_show_image(&avatar);
    ui_profile_set_battery(86, 5);
    screenshot("home");
    ui_profile_set_ble_connected(true); lv_tick_inc(1000); screenshot("home-bluetooth-connected");
    if (argc == 4) {
        for (unsigned i = 1; i <= 8; ++i) {
            char path[1024], name[40];
            snprintf(path, sizeof(path), "%s/avatar-%02u.bin", argv[3], i);
            load_avatar(path); lv_image_cache_drop(&avatar);
            ui_profile_show_image(&avatar);
            snprintf(name, sizeof(name), "home-avatar-%02u", i); screenshot(name);
        }
        /* Match the opaque JPEG descriptor and its native 96x160 crop path. */
        static uint16_t photo_pixels[96 * 160];
        for (unsigned y = 0; y < 160; ++y)
            for (unsigned x = 0; x < 96; ++x)
                photo_pixels[y * 96 + x] = y < 2 || y >= 158 ? 0xf800 : 0x4c37;
        static lv_image_dsc_t photo = {.header = {.magic=LV_IMAGE_HEADER_MAGIC,
            .cf=LV_COLOR_FORMAT_RGB565, .w=96, .h=160, .stride=192},
            .data_size=sizeof(photo_pixels), .data=(const uint8_t *)photo_pixels};
        ui_profile_show_image(&photo); screenshot("home-opaque-photo");
        ui_profile_show_image(NULL); screenshot("home-placeholder");
        load_avatar(argv[2]); lv_image_cache_drop(&avatar);
        ui_profile_show_image(&avatar); screenshot("home-alpha-restored");
        return 0;
    }
    xz_view_t voice = {.phase=XZ_READY,.avatar=0,.revision=1};
    strcpy(voice.detail,"你好，今天想聊些什么？");
    // Actual dedicated-boot screen hierarchy: hidden profile, suspended Plays
    // owner, then XiaoZhi. Even retain the decoded profile here as a worst case.
    assert(ui_boot_open());lv_tick_inc(940);screenshot("startup-brand");
    ui_plays_open(NULL,NULL,NULL);ui_plays_suspend();
    xz_view_t boot_voice=voice;boot_voice.phase=XZ_BOOT;
    strcpy(boot_voice.detail,"正在准备连接");
    ui_xiaozhi_open(&boot_voice,voice_art(0));
    screenshot("startup-brand-over-voice");ui_boot_close();
    screenshot("xiaozhi-boot");check_label_bounds(lv_screen_active());
    boot_voice.phase=XZ_WELCOME;boot_voice.revision++;ui_xiaozhi_set_battery(72,4);
    ui_xiaozhi_update(&boot_voice,voice_art(0));screenshot("xiaozhi-welcome");check_label_bounds(lv_screen_active());
    ui_xiaozhi_close();ui_plays_close();
    ui_xiaozhi_open(&voice,voice_art(0)); screenshot("xiaozhi-ready"); check_label_bounds(lv_screen_active());
    xz_view_t standby = voice;
    standby.phase = XZ_STANDBY;standby.revision++;
    strcpy(standby.detail,"按对话开始聊天");
    ui_xiaozhi_update(&standby,voice_art(0));screenshot("xiaozhi-standby");check_label_bounds(lv_screen_active());
    ui_xiaozhi_update(&voice,voice_art(0));
    if(getenv("FACE_ANIMATION_PREVIEW")) {
        for(unsigned person=0;person<2;person++) {
            voice.avatar=person;voice.phase=XZ_READY;voice.revision++;
            ui_xiaozhi_update(&voice,voice_art(person));
            for(unsigned tick=0;tick<72;tick++) {
                if(tick==40){voice.phase=XZ_LISTEN;voice.revision++;strcpy(voice.detail,"聆听中。。。");}
                if(tick==48){voice.phase=XZ_THINK;voice.revision++;strcpy(voice.detail,"让我想一想…");}
                if(tick==56){voice.output_active=true;voice.phase=XZ_SPEAK;voice.revision++;strcpy(voice.detail,"今天也有值得期待的小事。一起去发现吧！");}
                ui_xiaozhi_update(&voice,voice_art(person));
                char shot[80];snprintf(shot,sizeof shot,"face-%u-%03u",person,tick);
                lv_tick_inc(60);screenshot(shot); // screenshot advances the remaining 40ms.
            }
            ui_xiaozhi_close();voice.phase=XZ_READY;voice.revision++;
            strcpy(voice.detail,"你好，今天想聊些什么？");
            if(person==0)ui_xiaozhi_open(&voice,voice_art(1));
        }
        return 0;
    }

    voice.phase=XZ_LISTEN; voice.revision++; strcpy(voice.detail,"聆听中。。。");
    ui_xiaozhi_update(&voice,voice_art(0)); screenshot("xiaozhi-listening");
    voice.output_active=true;voice.phase=XZ_SPEAK; voice.revision++; strcpy(voice.detail,"今天也有值得期待的小事。一起去发现吧！");
    ui_xiaozhi_update(&voice,voice_art(0)); screenshot("xiaozhi-speaking");
    voice.revision++; strcpy(voice.detail,"哦，呀！嗯嗯，嗨～\n呦、诶、噢、喔、嘞、嘟。");
    ui_xiaozhi_update(&voice,voice_art(0)); screenshot("xiaozhi-complete-fonts");
    voice.volume=95;voice.natural_interrupt=false;voice.revision++;
    ui_xiaozhi_update(&voice,voice_art(0));
    unsigned setting_value=0;
    ui_xiaozhi_key(UI_PLAY_UP,&setting_value);ui_xiaozhi_key(UI_PLAY_OK,&setting_value);
    screenshot("xiaozhi-settings");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_DOWN,&setting_value);ui_xiaozhi_key(UI_PLAY_DOWN,&setting_value);
    ui_xiaozhi_key(UI_PLAY_OK,&setting_value);screenshot("xiaozhi-interrupt-mode");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);ui_xiaozhi_key(UI_PLAY_UP,&setting_value);
    ui_xiaozhi_key(UI_PLAY_OK,&setting_value);screenshot("xiaozhi-volume");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);
    ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);
    for(int i=0;i<3;i++)ui_xiaozhi_key(UI_PLAY_DOWN,&setting_value);
    ui_xiaozhi_key(UI_PLAY_OK,&setting_value);screenshot("xiaozhi-agent");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);
    ui_xiaozhi_key(UI_PLAY_DOWN,&setting_value);
    ui_xiaozhi_key(UI_PLAY_OK,&setting_value);screenshot("xiaozhi-brightness");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);ui_xiaozhi_key(UI_PLAY_DOWN,&setting_value);
    screenshot("xiaozhi-settings-scrolled");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_OK,&setting_value);screenshot("xiaozhi-sleep-time");check_label_bounds(lv_screen_active());
    ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);ui_xiaozhi_key(UI_PLAY_BACK,&setting_value);
    voice.phase=XZ_READY; voice.avatar=1; voice.revision++;
    strcpy(voice.detail,"你好，今天想聊些什么？");
    ui_xiaozhi_update(&voice,voice_art(1)); screenshot("xiaozhi-male");
    voice.phase=XZ_SETUP; voice.revision++; strcpy(voice.detail,"");
    strcpy(voice.hotspot,"AI-PASSPORT-ABCD"); voice.password[0]=0;
    ui_xiaozhi_update(&voice,voice_art(1)); screenshot("xiaozhi-setup"); check_label_bounds(lv_screen_active());
    voice.phase=XZ_BIND; voice.revision++; strcpy(voice.detail,"联网成功，请前往 xiaozhi.me，登录并进入控制台，添加设备时输入 123456 绑定。"); strcpy(voice.code,"123456");
    ui_xiaozhi_update(&voice,voice_art(1)); screenshot("xiaozhi-binding"); check_label_bounds(lv_screen_active());
    ui_xiaozhi_close();
    if (getenv("XIAOZHI_PREVIEW_ONLY")) {
        ui_plays_open(NULL, NULL, NULL); screenshot("xiaozhi-plays");
        for(unsigned i=1;i<7;i++) {
            ui_plays_menu_key(UI_PLAY_DOWN);
            char name[40];snprintf(name,sizeof name,"plays-position-%u",i+1);screenshot(name);
        }
        ui_plays_close();
        return 0;
    }
    ui_profile_set_name("𤩽珺璟"); screenshot("home-nickname-extended");
    ui_profile_set_name("博博的旅伴");
    lv_obj_t *profile = lv_screen_active();
    ui_album_no_photo(profile); screenshot("album-empty");
    ui_album_button(2, UI_ALBUM_PRESS); ui_album_button(2, UI_ALBUM_CLICK);
    lv_obj_t *photo = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(photo, lv_color_hex(0x34647C), 0);
    lv_obj_set_style_bg_opa(photo, LV_OPA_COVER, 0);
    lv_screen_load(photo); ui_album_attach(photo); screenshot("album-photo-no-overlay");
    ui_album_button(2, UI_ALBUM_PRESS); ui_album_button(2, UI_ALBUM_CLICK); screenshot("album-return-menu");
    ui_album_button(1, UI_ALBUM_PRESS); ui_album_button(1, UI_ALBUM_CLICK); screenshot("album-cancel-selected");
    ui_album_button(2, UI_ALBUM_PRESS); ui_album_button(2, UI_ALBUM_CLICK);
    lv_screen_load(profile); lv_obj_delete(photo);
    assert(ui_licenses_open()); screenshot("font-licenses");
    check_label_bounds(lv_screen_active());
    ui_licenses_close(); lv_tick_inc(40); lv_timer_handler();
    assert(lv_screen_active() == profile);
    /* Synthetic identity only: preview QR must never contain a real device key. */
    ui_button_guide_open("PREVIEW01", "PREVIEW_ONLY");
    screenshot("boot-guide"); check_label_bounds(lv_screen_active());
    ui_button_guide_close(); lv_tick_inc(40); lv_timer_handler();
    ui_button_guide_open("PREVIEW01234", "&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&&");
    screenshot("boot-guide-long"); check_label_bounds(lv_screen_active());
    ui_button_guide_close(); lv_tick_inc(40); lv_timer_handler();
    assert(lv_screen_active() == profile);
    ui_plays_open(NULL, NULL, NULL);
    screenshot("plays-menu");
    for (unsigned i = 0; i < 4; ++i) ui_plays_menu_key(UI_PLAY_DOWN);
    screenshot("plays-connect-selected"); check_label_bounds(lv_screen_active());
    lv_obj_t *plays = lv_screen_active();
    ui_button_guide_open_menu("PREVIEW01", "PREVIEW_ONLY");
    screenshot("plays-connect-qr"); check_label_bounds(lv_screen_active());
    ui_button_guide_close(); lv_tick_inc(40); lv_timer_handler();
    assert(lv_screen_active() == plays);
    ui_plays_menu_key(UI_PLAY_DOWN); screenshot("plays-home-selected");
    lv_mem_monitor_t qr_first, qr_last;
    for (unsigned i = 0; i <= 100; ++i) {
        ui_button_guide_open_menu("PREVIEW01", "PREVIEW_ONLY");
        lv_refr_now(NULL); ui_button_guide_close(); lv_tick_inc(40); lv_timer_handler();
        assert(lv_screen_active() == plays);
        if (i == 0) lv_mem_monitor(&qr_first);
    }
    lv_mem_monitor(&qr_last); assert(qr_first.free_size == qr_last.free_size);
    ab_session_view_t answer = {.state = AB_STATE_IDLE};
    ui_plays_answer(&answer, 0, 0); screenshot("answer-idle");
    answer.state = AB_STATE_LISTENING;
    ui_plays_answer(&answer, 600, 2000); screenshot("answer-listening");
    answer.state = AB_STATE_PAGE_TURN; answer.deadline_ms = 3900;
    ui_plays_answer(&answer, 0, 3500); screenshot("answer-turning");
    answer.state = AB_STATE_ANSWER; answer.answer_ready = true; answer.answer_index = 23;
    ui_plays_answer(&answer, 0, 0); screenshot("answer-result");
    ui_click_view_t pairing = {.status = UI_CLICK_PAIRING, .name = "AI PASSPORT Click"};
    ui_plays_clicker(&pairing); screenshot("clicker-purpose");
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_COMPUTER_MODE);
    screenshot("clicker-pairing");
    ui_click_view_t click = {.status = UI_CLICK_READY, .sent = 24, .name = "AI PASSPORT Click"};
    ui_plays_clicker(&click); screenshot("clicker-ready");
    ui_plays_clicker_key(UI_PLAY_OK); screenshot("clicker-menu-page1");
    for (int i = 0; i < 3; ++i) ui_plays_clicker_key(UI_PLAY_DOWN);
    screenshot("clicker-menu-page2");
    ui_plays_clicker_key(UI_PLAY_DOWN); ui_plays_clicker_key(UI_PLAY_DOWN); screenshot("clicker-change-selected");
    ui_plays_clicker_key(UI_PLAY_OK); screenshot("clicker-change-instructions");
    ui_plays_clicker_key(UI_PLAY_BACK);
    ui_plays_clicker_key(UI_PLAY_DOWN); screenshot("clicker-return");
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_EXIT);
    ui_plays_clicker_key(UI_PLAY_BACK);
    ui_plays_clicker_key(UI_PLAY_OK); ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_NONE);
    ui_plays_clicker_key(UI_PLAY_DOWN);
    assert(ui_plays_clicker_key(UI_PLAY_OK) == UI_CLICK_ACTION_PHONE_MODE);
    click.phone_mode = true; click.status = UI_CLICK_PAIRING;
    ui_plays_clicker(&click); screenshot("clicker-phone-pairing"); check_label_bounds(lv_screen_active());
    click.status = UI_CLICK_READY; ui_plays_clicker(&click); screenshot("clicker-phone-ready"); check_label_bounds(lv_screen_active());
    ui_plays_clicker_key(UI_PLAY_OK); screenshot("clicker-phone-menu");
    ui_plays_clicker_key(UI_PLAY_UP); screenshot("clicker-phone-return");
    ui_plays_suspend();
    ui_game_open(NULL, NULL); screenshot("dodge-ready");
    game_key(UI_GAME_KEY_ENTER);
    for (int i = 0; i < 27; ++i) { lv_tick_inc(40); lv_timer_handler(); }
    screenshot("dodge-running");
    game_key(UI_GAME_KEY_ENTER); game_key(UI_GAME_KEY_DOWN); game_key(UI_GAME_KEY_DOWN);
    screenshot("dodge-paused-return");
    game_key(UI_GAME_KEY_ENTER); assert(!ui_game_is_active());
    ui_plays_close();
    if (!getenv("SPLIT_FACTORY_BOOT")) render_selftest(profile);
    return 0;
}
