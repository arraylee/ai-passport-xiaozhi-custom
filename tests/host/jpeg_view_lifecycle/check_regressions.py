"""Prove each protection matters by changing only the generated build copy."""
from pathlib import Path
import subprocess
import sys


def run(*args, expect_success=True):
    result = subprocess.run(args, capture_output=True, text=True)
    if expect_success and result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    return result


build = Path(sys.argv[1]).resolve()
generated = build / 'jpeg_view_production.inc'
binary = build / 'test_jpeg_view_lifecycle'
command = ['cmake', '--build', str(build), '--target', 'test_jpeg_view_lifecycle', '-j', '8']
run(*command)


def rebuild_mutation():
    # macOS Make can consider successive header writes in the same second
    # unchanged. Rebuild this small translation unit for each mutation, so a
    # stale binary cannot produce a false detection (or a false pass).
    for obj in (build / 'CMakeFiles/test_jpeg_view_lifecycle.dir').rglob('*.o'):
        obj.unlink()
    binary.unlink(missing_ok=True)
    run(*command)


original = generated.read_text()
regressions = [
    ('skip blocked-screen ownership', 'deferred',
     'if (s_can_show_cb && !s_can_show_cb(s_can_show_user)) return;',
     '/* intentionally remove the can-show guard in the generated test copy */',
     'blocked fullscreen request decoded or displayed'),
    ('remember replaced photo as home', 'replace',
     'if (!old_scr) s_prev = lv_screen_active();', 's_prev = lv_screen_active();',
     'fullscreen return target is not the original home'),
    ('leak replaced photo screen', 'replace',
     'if (old_scr && old_scr != s_scr) lv_obj_delete(old_scr);', '(void)old_scr;',
     'replacement did not delete the actual old LVGL photo screen'),
    ('leave invalid frame after replacement failure', 'decode_failure',
     '        teardown();\n        return -1;', '        return -1;',
     'failed replacement left the old LVGL photo referencing invalid frame memory'),
    ('skip first-failure avatar cleanup', 'first_decode_failure',
     '        teardown();\n        return -1;', '        if (old_scr) teardown();\n        return -1;',
     'failed first photo left the existing home avatar on invalid frame memory'),
    ('overwrite visible photo with avatar', 'active_named_avatar',
     'if (s_pending && !s_active)', 'if (s_pending)',
     'avatar update overwrote the shared frame while a photo was visible', 2),
    ('discard waiting photo on avatar update', 'blocked_full_with_avatar',
     '    s_restore_only = 1;\n    s_pending = 1;',
     '    s_restore_only = 1;\n    s_pending = 1; s_full_pending = 0;',
     'an intervening avatar update consumed the blocked full-photo request'),
    ('overwrite explicit avatar notification ownership', 'active_uploaded_avatar',
     'if (!s_pending && ((', 'if (((',
     "explicit uploaded avatar lost its result or named avatar sent an unsolicited result"),
    ('defer return-avatar restore to the next tick', 'seamless_return',
     '        teardown();\n    }\n    // Avatar changes',
     '        teardown();\n        return;\n    }\n    // Avatar changes',
     'first home refresh exposed the placeholder before restoring the saved avatar'),
]
try:
    for label, scenario, before, after, expected, *counts in regressions:
        # Avatar restoration is guarded both at tick and in its helper; the
        # mutation must remove both defenses to reproduce the original fault.
        count = counts[0] if counts else 1
        if original.count(before) != count:
            raise RuntimeError(f'Cannot apply one precise regression to current production code: {label}')
        mutation = original.replace(before, after, count)
        generated.write_text(mutation)
        rebuild_mutation()
        if generated.read_text() != mutation:
            raise RuntimeError(f'Build overwrote the generated mutation: {label}')
        result = run(str(binary), scenario, expect_success=False)
        output = result.stdout + result.stderr
        if result.returncode == 0 or expected not in output:
            raise RuntimeError(f'Regression not detected as expected: {label}\n{output}')
        print(f'DETECTED: {label}', flush=True)
finally:
    generated.write_text(original)
    rebuild_mutation()
run('ctest', '--test-dir', str(build), '--output-on-failure')
print('Restored exact production slice; all normal lifecycle scenarios passed.')
