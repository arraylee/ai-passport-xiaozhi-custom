"""Compile the actual boot UI block, avatar callback and first-frame gate on host."""
from pathlib import Path
import sys


def block(text, marker):
    start = text.index(marker)
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end] + '\n'


app, platform, target = map(Path, sys.argv[1:])
a, p = app.read_text(), platform.read_text()
parts = [p[p.index('typedef struct {'):p.index('// 由 disp_st7789.c')]]
parts += [block(p, 'static void first_frame_ready('), block(p, 'bool platform_lvgl_arm_first_frame(')]
if 'static struct _lv_display_t *s_wake_display;' in a:
    parts += ['static struct _lv_display_t *s_wake_display;\nstatic atomic_bool s_boot_avatar_ready;\n', block(a, 'static void finish_wake_frame(')]
parts += [block(a, 'static void on_jpeg_avatar(')]
parts += ['static void boot_profile(bool woke_from_deep_sleep, lv_display_t *lvdisp) {\n'
          'bool first_frame_armed = false;\n'
          + block(a, 'if (platform_lvgl_lock(0)) {                 // 建界面')
          + 'assert(first_frame_armed);\n}\n']
if 'static void restore_boot_avatar(' in a:
    parts += ['#define HAS_BOOT_RESTORE 1\n', block(a, 'static void restore_boot_avatar(void) {')]
else:
    parts += ['#define HAS_BOOT_RESTORE 0\n']
parts += [block(a, 'static void show_boot_button_guide(void) {')]
target.write_text('/* Exact production fragments; only hardware boundaries are faked. */\n' + '\n'.join(parts))
