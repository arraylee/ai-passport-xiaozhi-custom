"""Expose actual platform clipping and draw-buffer dimensions to native tests."""
from pathlib import Path
import re
import sys

source, target = map(Path, sys.argv[1:])
text = source.read_text()
start = text.index('static void round_active_screen(')
end = text.index('\nlv_display_t *platform_lvgl_init(', start)
rounding = text[start:end]
rows = re.search(r'\.buffer_size\s*=\s*\(uint32_t\)cfg->lcd_w\s*\*\s*(\d+)\s*,', text)
assert rows and '.double_buffer = true' in text
assert 'lv_display_add_event_cb(d, round_active_screen, LV_EVENT_SCREEN_LOADED, NULL);' in text
target.write_text('#include "platform/board_config.h"\n'
                  + '#define PASSPORT_DISPLAY_ROWS ' + rows[1] + '\n' + rounding)
