"""Generate independent uncompressed native-pixel checksums for every UI glyph.

This is a host-test fixture, never linked into firmware. Regenerate only when
the source font/character contract changes; runtime tests decode the real pack.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import zlib
try:
    from . import gen_cn_font as g
except ImportError:
    import gen_cn_font as g

DEST = g.ROOT / 'tests/host/ui/font_oracle.bin'


def generate():
    ui = g.ui_fonts()
    records, counts = bytearray(), {}
    with tempfile.TemporaryDirectory() as tmp:
        for slot, size, fast in [(16, 16, 'ui_font_body'), (24, 20, 'ui_font_title')]:
            chars = sorted(g.historical_sets()[slot] | ui[fast])
            counts[str(size)] = len(chars)
            for start in range(0, len(chars), g.PART_GLYPHS):
                subset = chars[start:start+g.PART_GLYPHS]
                _, native = g.convert('oracle', size, subset, Path(tmp), raw=True)
                for c, metric in zip(subset, native['glyphs'][1:]):
                    begin, pixels = metric['bitmap_index'], metric['box_w'] * metric['box_h']
                    raw = native['bitmap'][begin:begin+(pixels+1)//2]
                    a8 = bytes(((raw[i//2] >> (0 if i & 1 else 4)) & 15) * 17 for i in range(pixels))
                    records += struct.pack('<IHHBBbbI', c, size, (metric['adv_w']+8)//16,
                        metric['box_w'], metric['box_h'], metric['ofs_x'], metric['ofs_y'], zlib.crc32(a8))
    data = struct.pack('<4sI', b'FTO1', len(records)//16) + records
    DEST.write_bytes(data)
    DEST.with_suffix('.json').write_text(json.dumps(dict(source_sha256=g.SOURCE_SHA256,
        converter='lv_font_conv@1.5.3 --no-compress --bpp 4 --no-kerning',
        counts=counts, sha256=hashlib.sha256(data).hexdigest()), indent=2)+'\n')
    print(f'Independent native pixel oracle: {counts}, {len(data)} bytes')


def check():
    data = DEST.read_bytes(); meta = json.loads(DEST.with_suffix('.json').read_text())
    assert meta['source_sha256'] == g.SOURCE_SHA256
    assert hashlib.sha256(data).hexdigest() == meta['sha256']
    magic, count = struct.unpack_from('<4sI', data)
    assert magic == b'FTO1' and len(data) == 8 + count*16
    actual = {16:set(), 20:set()}
    for at in range(8, len(data), 16):
        c, size = struct.unpack_from('<IH', data, at); actual[size].add(c)
    ui = g.ui_fonts()
    assert actual[16] == g.historical_sets()[16] | ui['ui_font_body']
    assert actual[20] == g.historical_sets()[24] | ui['ui_font_title']
    print('Native pixel oracle source/hash/complete coverage PASS')


if __name__ == '__main__':
    p = argparse.ArgumentParser(); p.add_argument('--check', action='store_true')
    args = p.parse_args()
    check() if args.check else generate()
