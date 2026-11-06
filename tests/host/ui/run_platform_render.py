"""Render the retained screen sequence with real packs and platform clipping."""
from pathlib import Path
import os
import re
import subprocess
import sys
from PIL import Image

executable, root, output = map(Path, sys.argv[1:])
output.mkdir(parents=True, exist_ok=True)
avatar = output / 'avatar.bgra'
with Image.open(root / 'assets/avatars/avatar-01.png') as image:
    assert image.size == (96, 156), image.size
    avatar.write_bytes(image.convert('RGBA').tobytes('raw', 'BGRA'))
pack = root / 'build/companion/companion.bin'
assert pack.is_file(), 'Build the firmware resource pack before this test'
env = dict(os.environ, COMPANION_PACK=str(pack), SPLIT_FACTORY_BOOT='1')
scenes = set()
for mode in ('normal', 'factory'):
    if mode == 'factory': env['SELFTEST_PREVIEW_ONLY'] = '1'
    log = output / (mode + '-render.log')
    try:
        result = subprocess.run([str(executable), str(output), str(avatar)], env=env,
                                capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as error:
        log.write_bytes((error.stdout or b'')[-16000:] + (error.stderr or b'')[-16000:])
        raise
    log.write_text(result.stdout + result.stderr)
    print(result.stdout[-6000:] + result.stderr[-1000:])
    assert result.returncode == 0, mode + ': rounded retained-screen rendering failed'
    scenes.update(re.findall(r'^([a-z0-9-]+): current=', result.stdout, re.M))
assert len(scenes) >= 41 and {'selftest-progress', 'selftest-result', 'xiaozhi-settings', 'xiaozhi-interrupt-mode', 'xiaozhi-volume', 'xiaozhi-welcome', 'xiaozhi-agent', 'xiaozhi-brightness', 'xiaozhi-sleep-time', 'xiaozhi-settings-scrolled', 'startup-brand-over-voice'} <= scenes, 'missing current-run screens'
# The empty theme screen uses LVGL's full subtree clip. Compare all exterior
# and antialiased edge pixels of the XiaoZhi screens against that reference.
with Image.open(output / 'xiaozhi-rounded-reference.ppm') as reference:
    ref = reference.convert('RGB')
for name in sorted(s for s in scenes if s.startswith('xiaozhi-')):
    with Image.open(output / (name+'.ppm')) as frame:
        for y in range(30):
            for x in range(30):
                if (29.5-x)**2 + (29.5-y)**2 < 30**2: continue
                for xx in (x,239-x):
                    for yy in (y,319-y):
                        assert frame.getpixel((xx,yy)) == ref.getpixel((xx,yy)), (name,xx,yy)
print('Actual rounded screens and8-row double buffers: native retained rendering PASS')
