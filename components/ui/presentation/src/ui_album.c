#include "presentation/ui_album.h"
#include "presentation/ui_theme.h"
#include <stdatomic.h>
#include <string.h>

enum { ALBUM_CLOSED, ALBUM_PHOTO, ALBUM_NOTICE, ALBUM_MENU };
static atomic_uint mode;
static struct {
    lv_obj_t *owner, *panel, *options[2];
    unsigned selected, pressed;
} A;

static void owner_deleted(lv_event_t *event) {
    if (lv_event_get_target(event) != A.owner) return;
    atomic_store(&mode, ALBUM_CLOSED);
    memset(&A, 0, sizeof A);
}
static void hide_panel(void) {
    if (A.panel) lv_obj_delete(A.panel);
    A.panel = NULL;
    A.options[0] = A.options[1] = NULL;
}
static void attach(lv_obj_t *owner, unsigned next) {
    hide_panel();
    if (A.owner) lv_obj_remove_event_cb(A.owner, owner_deleted);
    memset(&A, 0, sizeof A);
    A.owner = owner;
    lv_obj_add_event_cb(owner, owner_deleted, LV_EVENT_DELETE, NULL);
    atomic_store(&mode, next);
}
void ui_album_attach(void *screen) { attach(screen, ALBUM_PHOTO); }
bool ui_album_active(void) { return atomic_load(&mode) != ALBUM_CLOSED; }

static void selected_style(void) {
    for (unsigned i = 0; i < 2; ++i) {
        bool selected = i == A.selected;
        lv_obj_set_style_border_color(A.options[i], lv_color_hex(selected ? UI_ACCENT : UI_LINE), 0);
        lv_obj_set_style_border_width(A.options[i], selected ? 2 : 1, 0);
    }
}
static void show_menu(void) {
    A.selected = 0;
    A.panel = ui_theme_panel(A.owner, 16, 92, 208, 150, false);
    ui_theme_text(A.panel, "相册", 16, 12, 174, &ui_font_title, UI_INK);
    const char *labels[] = {"返回首页", "取消"};
    for (unsigned i = 0; i < 2; ++i) {
        A.options[i] = ui_theme_panel(A.panel, 14, 48 + i * 45, 180, 37, i == 0);
        lv_obj_t *label = ui_theme_text(A.options[i], labels[i], 10, 7, 156, &ui_font_body, UI_INK);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    }
    atomic_store(&mode, ALBUM_MENU);
}
void ui_album_no_photo(void *home) {
    if (!home) home = lv_screen_active();
    attach(home, ALBUM_NOTICE);
    A.panel = ui_theme_panel(home, 16, 84, 208, 162, false);
    ui_theme_text(A.panel, "相册", 16, 12, 174, &ui_font_title, UI_INK);
    lv_obj_t *message = ui_theme_text(A.panel, "暂无照片，请通过小程序上传", 16, 48, 174, &ui_font_body, UI_INK);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *button = ui_theme_panel(A.panel, 24, 112, 160, 35, true);
    lv_obj_t *label = ui_theme_text(button, "知道了", 8, 6, 144, &ui_font_body, UI_INK);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}
ui_album_action_t ui_album_button(int index, ui_album_event_t event) {
    unsigned current = atomic_load(&mode);
    if (index < 0 || index > 2 || current == ALBUM_CLOSED) return UI_ALBUM_NONE;
    unsigned bit = 1u << index;
    if (event == UI_ALBUM_PRESS) { A.pressed |= bit; return UI_ALBUM_NONE; }
    /* The homepage's release may produce a delayed CLICK after this view
     * opens. A new view accepts only gestures whose PRESS it has seen. */
    if (!(A.pressed & bit)) return UI_ALBUM_NONE;
    A.pressed &= ~bit;
    if (current == ALBUM_NOTICE) {
        if (index != 2) return UI_ALBUM_NONE;
        hide_panel();
        lv_obj_remove_event_cb(A.owner, owner_deleted);
        A.owner = NULL;
        atomic_store(&mode, ALBUM_CLOSED);
        return UI_ALBUM_DISMISSED;
    }
    if (current == ALBUM_PHOTO) {
        if (index == 2) show_menu();
        return UI_ALBUM_NONE;
    }
    if (event == UI_ALBUM_LONG && index == 2) {
        hide_panel(); atomic_store(&mode, ALBUM_PHOTO); return UI_ALBUM_NONE;
    }
    if (index < 2) { A.selected ^= 1u; selected_style(); return UI_ALBUM_NONE; }
    unsigned selected = A.selected;
    hide_panel(); atomic_store(&mode, ALBUM_PHOTO);
    return selected == 0 ? UI_ALBUM_RETURN_HOME : UI_ALBUM_NONE;
}
