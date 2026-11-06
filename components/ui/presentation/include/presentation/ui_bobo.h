#pragma once
#include "services/bobo.h"
#include "services/bobo_save.h"
#include "services/pet_wish.h"

#include "presentation/ui_play_key.h"
typedef enum {
    UI_BOBO_CARE, UI_BOBO_EQUIP, UI_BOBO_TARGET, UI_BOBO_START,
    UI_BOBO_CANCEL, UI_BOBO_CLAIM, UI_BOBO_RETRY_SAVE,
    UI_BOBO_WISH_START, UI_BOBO_WISH_CANCEL, UI_BOBO_SELECT_PET, UI_BOBO_REVEAL_ACK
} ui_bobo_command_t;
typedef bool (*ui_bobo_command_cb_t)(ui_bobo_command_t command, unsigned argument, void *user);
typedef void (*ui_bobo_exit_cb_t)(void *user);

// LVGL context/lock required for all calls. Callbacks must enqueue, never write Flash.
void ui_bobo_open(const bobo_state_t *state, bobo_load_result_t load_status,
                  bool clock_trusted, ui_bobo_command_cb_t command,
                  ui_bobo_exit_cb_t exit, void *user);
void ui_bobo_close(void);
bool ui_bobo_is_active(void);
// Freeze the native pet frames before display sleep; resume after wake input is consumed.
void ui_bobo_suspend(bool suspended);
void ui_bobo_key(ui_play_key_t key);
// For clock updates acknowledgement=false. A command acknowledgement clears pending input.
// save_ok=false means the action is not confirmed; present retry with retained state.
void ui_bobo_update(const bobo_state_t *state, bobo_result_t result,
                    bool save_ok, bool acknowledgement, bool clock_trusted);
// Publish committed bobo state first. ACCEPTED alone never reveals an uncommitted pet.
void ui_bobo_wish_update(const pet_wish_view_t *view, bool save_ok);
