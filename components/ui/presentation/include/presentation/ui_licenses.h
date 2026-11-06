#pragma once
#include <stdbool.h>
#include <stddef.h>

/* All calls require the LVGL context/lock. Transitions are deferred until the
 * current input event finishes; no app-side key routing is required. */
bool ui_licenses_open(void);
void ui_licenses_close(void);
bool ui_licenses_is_active(void);

typedef struct {
    size_t offset, end; /* Original Flash text byte range, including newlines. */
    unsigned page, pages; /* One-based displayed page and total. */
} ui_licenses_view_t;
/* Read-only diagnostics; false before the deferred open or after close. */
bool ui_licenses_get_view(ui_licenses_view_t *view);
