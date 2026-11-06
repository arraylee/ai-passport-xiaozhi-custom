#pragma once
#include <stdbool.h>

typedef enum { UI_ALBUM_PRESS, UI_ALBUM_CLICK, UI_ALBUM_LONG } ui_album_event_t;
typedef enum { UI_ALBUM_NONE, UI_ALBUM_DISMISSED, UI_ALBUM_RETURN_HOME } ui_album_action_t;
/* Mutations require the LVGL lock. Full-screen photos have no permanent text. */
void ui_album_attach(void *screen);
void ui_album_no_photo(void *home);
bool ui_album_active(void);
ui_album_action_t ui_album_button(int index, ui_album_event_t event);
