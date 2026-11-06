#pragma once
#include "presentation/ui_play_key.h"
#include "services/answer_session.h"

typedef enum { UI_PLAY_ANSWER, UI_PLAY_CLICKER, UI_PLAY_DODGE, UI_PLAY_XIAOZHI, UI_PLAY_CONNECT } ui_play_id_t;
typedef bool (*ui_play_select_cb_t)(ui_play_id_t play, void *user);
typedef void (*ui_plays_exit_cb_t)(void *user);
typedef enum { UI_CLICK_OFF, UI_CLICK_PAIRING, UI_CLICK_CONNECTING, UI_CLICK_READY, UI_CLICK_UNAVAILABLE, UI_CLICK_SWITCHING } ui_click_status_t;
typedef struct { ui_click_status_t status; uint32_t sent, failed; const char *name; bool phone_mode, touch_mode, scroll_reversed; } ui_click_view_t;
typedef enum { UI_CLICK_ACTION_NONE, UI_CLICK_ACTION_PREV, UI_CLICK_ACTION_NEXT,
    UI_CLICK_ACTION_START, UI_CLICK_ACTION_END, UI_CLICK_ACTION_BLANK,
    UI_CLICK_ACTION_CHANGE_COMPUTER, UI_CLICK_ACTION_EXIT, UI_CLICK_ACTION_SWITCH_MODE,
    UI_CLICK_ACTION_PHONE_MODE, UI_CLICK_ACTION_COMPUTER_MODE, UI_CLICK_ACTION_TOUCH_MODE, UI_CLICK_ACTION_REVERSE_SCROLL,
    UI_CLICK_ACTION_POINTER_TEST,
    UI_CLICK_ACTION_PLAY = UI_CLICK_ACTION_START } ui_click_action_t;

// All calls require the LVGL context/lock. Selection callback only enqueues work.
void ui_plays_open(ui_play_select_cb_t select, ui_plays_exit_cb_t exit, void *user);
void ui_plays_close(void);
bool ui_plays_is_active(void);
void ui_plays_menu(void);
void ui_plays_menu_key(ui_play_key_t key);
void ui_plays_suspend(void); // Keep only the root while a separate child screen is active.
void ui_plays_answer(const ab_session_view_t *view, uint16_t level, uint64_t now_ms);
void ui_plays_clicker(const ui_click_view_t *view);
// Short OK opens the visible action menu. No presentation action is hidden behind
// a long press or double click. The app sends only the returned semantic action.
ui_click_action_t ui_plays_clicker_key(ui_play_key_t key);
void ui_plays_clicker_feedback(const char *action, bool queued);
