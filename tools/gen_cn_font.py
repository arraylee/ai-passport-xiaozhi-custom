#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate OFL-licensed Passport fallbacks with complete historical nickname coverage.

Legacy public symbols lv_font_cn_16 / lv_font_cn_24 now mean native 16px / 20px.
The five UI subsets remain the fast path; rare body glyphs use LVGL lossless RLE,
and rare nickname/reply glyphs use random-access512-byte zlib blocks at4bpp.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
import tempfile
import zlib
from PIL import ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT_PATH = ROOT / "assets/fonts/LXGWWenKai-Medium.ttf"
SOURCE_SHA256 = "d4bdeb38a39151d74d084cba5090f8cb7d20bf83eedb78c35939ae70b9f4e3f6"
LICENSE_PATH = ROOT / "assets/fonts/LXGW-v1.522-OFL.txt"
OUT_DIR = ROOT / "components/ui/presentation/src"
MANIFEST_PATH = ROOT / "assets/fonts/fallback-fonts.json"
BPP = 4
BLOCK_SIZE = 512
PART_GLYPHS = 3300

# Historical nickname contract. NICKNAME_FALLBACK_CHARS remains available to
# audit the old list; all glyphs now use the single pinned repository font.
NICKNAME_EXTRA_CHARS = (
    "珺鍠雲飂鎛琭汎佈柷敔吥昇嗏燚溦彧燊吳垚榮拝楿髙。、～！（）°μ…″"
    "喆鋆韡烋烜璟旻祎姞飔玥瑄崝珅翬珹悅炘赟贇渱璠珮劼芃甦镕爔昪禤暢玶淏崟"
    "𤩽熺偲竑暐棨钖熇珽翀嫄宬湲湧嫚濛堃啟愷旸㛃倞玙杍翚桪昫昞樑纮澔梽頔"
    "姮冮顥晞沄屾祐锜瑨焜曈詝飏浛瑀栯橦妘競琎敭鹍镠禕乂荍珵嶒辀昳軻璘棫"
    "皛麥璕漘栢奡·，•′¯"
)
NICKNAME_FALLBACK_CHARS = "•¯𤩽"
# Cloud replies contain colloquial words outside GB2312, even though they are
# absent from built-in menu strings. Keep both dynamic-text sizes in sync.
DIALOG_EXTRA_CHARS = "欸啰囧哒嘚嘚喆祎玥麽裏麼妳説吶喲囉噓唸"


def gb2312_hanzi(include_level2=False):
    """枚举 GB2312 汉字；可包含二级字(0xD8A1-0xF7FE)。"""
    chars = []
    hi_end = 0xF8 if include_level2 else 0xD8
    for hi in range(0xB0, hi_end):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            if len(ch) == 1 and 0x4E00 <= ord(ch) <= 0x9FA5:
                chars.append(ch)
    return chars


def nickname_hanzi():
    """返回 24px 昵称字体字符集（完整 GB2312 + 额外姓名用字/符号）。"""
    chars = gb2312_hanzi(include_level2=True)
    for ch in NICKNAME_EXTRA_CHARS:
        if ch not in chars:
            chars.append(ch)
    return chars


def split_sparse_cmap_chars(chars):
    """按 LVGL SPARSE_TINY 的 uint16_t 偏移范围拆分有序字符。"""
    groups = []
    for ch in chars:
        if not groups or ord(ch) - ord(groups[-1][0]) > 0xFFFF:
            groups.append([])
        groups[-1].append(ch)
    return groups


def array_data(code):
    clean = re.sub(r'/\*.*?\*/|//[^\n]*', '', code, flags=re.S)
    return {name: (ctype, body) for ctype, name, body in re.findall(
        r'(?:static\s+)?(?:LV_ATTRIBUTE_LARGE_CONST\s+)?const\s+(\w+)\s+(\w+)\[\]\s*=\s*\{(.*?)\n\};', clean, re.S)}


def numbers(body):
    return [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\b\d+\b', body)]


def packed_codepoints(code):
    arrays = array_data(code)
    result = set()
    for ctype, body in arrays.values():
        if ctype != 'lv_font_fmt_txt_cmap_t':
            continue
        for entry in re.findall(r'\{([^{}]*)\}', body):
            f = dict(re.findall(r'\.(\w+)\s*=\s*(\w+)', entry))
            start = int(f['range_start'])
            if f['unicode_list'] != 'NULL':
                result.update(start + n for n in numbers(arrays[f['unicode_list']][1]))
            elif f['glyph_id_ofs_list'] != 'NULL':
                result.update(start + i for i, n in enumerate(numbers(arrays[f['glyph_id_ofs_list']][1])) if i == 0 or n)
            else:
                result.update(range(start, start + int(f['range_length'])))
    return result


def parse_font(code):
    arrays = array_data(code)
    glyphs = [{k: int(v) for k, v in re.findall(r'\.(\w+)\s*=\s*(-?\d+)', row)}
              for row in re.findall(r'\{([^{}]*)\}', arrays['glyph_dsc'][1])]
    return dict(arrays=arrays, glyphs=glyphs, bitmap=bytes(numbers(arrays['glyph_bitmap'][1])),
                codepoints=sorted(packed_codepoints(code)),
                line_height=int(re.search(r'\.line_height\s*=\s*(\d+)', code)[1]),
                base_line=int(re.search(r'\.base_line\s*=\s*(\d+)', code)[1]))


def convert(name, size, chars, temp, raw=False):
    destination = temp / (name + '.c')
    command = ['npx', '--yes', 'lv_font_conv@1.5.3', '--font', str(FONT_PATH),
               '--size', str(size), '--bpp', '4', '--no-kerning', '--format', 'lvgl',
               '--lv-include', 'lvgl.h', '--symbols', ''.join(map(chr, chars)),
               '--output', str(destination)]
    if raw:
        command.append('--no-compress')
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(result.stderr or result.stdout)
    code = destination.read_text()
    parsed = parse_font(code)
    if parsed['codepoints'] != chars or len(parsed['glyphs']) != len(chars) + 1:
        raise RuntimeError(f'{name}: converted cmap/glyphs differ from requested charset')
    if raw:
        for i, glyph in enumerate(parsed['glyphs'][1:], 1):
            end = parsed['glyphs'][i + 1]['bitmap_index'] if i + 1 < len(parsed['glyphs']) else len(parsed['bitmap'])
            if end - glyph['bitmap_index'] != (glyph['box_w'] * glyph['box_h'] + 1) // 2:
                raise RuntimeError(f'{name}: unexpected raw 4bpp glyph padding')
    return code, parsed


def emit_array(ctype, name, values):
    rows = [', '.join(str(n) for n in values[i:i + 16]) for i in range(0, len(values), 16)]
    return f'static const {ctype} {name}[] = {{\n' + ',\n'.join('    ' + row for row in rows) + '\n};\n'


def metadata(parsed, prefix):
    # Canonical sparse maps keep indices directly ordered by Unicode and allow
    # the 16px metrics/index tables to live in mapped Flash. Do not preserve the
    # converter's optional mixture of dense/sparse tables across revisions.
    ctype, body = parsed['arrays']['glyph_dsc']
    result = f'static const {ctype} {prefix}_glyph_dsc[] = {{{body}\n}};\n'
    entries, gid = [], 1
    for i, chars in enumerate(split_sparse_cmap_chars(list(map(chr, parsed['codepoints'])))):
        start, end = ord(chars[0]), ord(chars[-1])
        name = f'{prefix}_unicode_list_{i}'
        result += emit_array('uint16_t', name, [ord(c) - start for c in chars])
        entries.append(f'    {{.range_start = {start}, .range_length = {end-start+1}, .glyph_id_start = {gid}, '
                       f'.unicode_list = {name}, .glyph_id_ofs_list = NULL, .list_length = {len(chars)}, '
                       '.type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY}')
        gid += len(chars)
    return result + f'static const lv_font_fmt_txt_cmap_t {prefix}_cmaps[] = {{\n' + ',\n'.join(entries) + '\n};\n'


def font_struct(symbol, prefix, height, baseline, fallback='NULL', block=False, private=False):
    return f'''{'static ' if private else ''}const lv_font_t {symbol} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = {'passport_font_get_bitmap' if block else 'lv_font_get_bitmap_fmt_txt'},
    .line_height = {height}, .base_line = {baseline},
    .subpx = LV_FONT_SUBPX_NONE, .static_bitmap = 0,
    .underline_position = -3, .underline_thickness = 1,
    .dsc = &{prefix}_dsc, .fallback = {fallback}, .user_data = NULL,
}};
'''


def fmt_fields(parsed, prefix, bitmap, format):
    count = len(split_sparse_cmap_chars(list(map(chr, parsed['codepoints']))))
    return f'''.glyph_bitmap = {bitmap}, .glyph_dsc = {prefix}_glyph_dsc,
        .cmaps = {prefix}_cmaps, .kern_dsc = NULL, .kern_scale = 0,
        .cmap_num = {count}, .bpp = 4, .kern_classes = 0, .bitmap_format = {format}'''


def font_header(family):
    return ('/*\nGenerated by tools/gen_cn_font.py. Do not edit font data.\n'
            f'Derivative font family: {family}\n'
            'Source acknowledgement: LXGW WenKai Medium v1.522.\n'
            f'Source SHA-256: {SOURCE_SHA256}\n'
            'This license applies to this font software and glyph data,\n'
            'not to the firmware application as a whole.\n\n'
            + '\n'.join(line.rstrip() for line in LICENSE_PATH.read_text().splitlines()) + '\n*/\n\n#include "lvgl.h"\n'
            '#if LVGL_VERSION_MAJOR != 9\n#error "Passport fonts require LVGL 9"\n#endif\n')


def ui_fonts():
    manifest = json.loads((ROOT / 'assets/fonts/ui-fonts.json').read_text())
    result = {}
    for item in manifest['fonts']:
        code = (OUT_DIR / (item['name'] + '.c')).read_text()
        actual = packed_codepoints(code)
        if actual != set(item['codepoints']):
            raise RuntimeError(f"{item['name']}: UI manifest and actual cmap differ")
        result[item['name']] = actual
    if len(result) != 5:
        raise RuntimeError('Expected all five current UI fonts')
    return result


def historical_sets():
    ascii = set(range(32, 127))
    complete = ascii | set(map(ord, nickname_hanzi() + list(DIALOG_EXTRA_CHARS)))
    return {16: complete, 24: complete}


def source_check():
    if hashlib.sha256(FONT_PATH.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise RuntimeError('Source differs from pinned WenKai v1.522; audit new source/license first')
    if not LICENSE_PATH.is_file():
        raise RuntimeError('Missing version-bound original OFL 1.1 text')


def check(output_dir=OUT_DIR, manifest_path=MANIFEST_PATH):
    source_check()
    ui, old = ui_fonts(), historical_sets()
    manifest = json.loads(manifest_path.read_text())
    if {row['legacy_slot'] for row in manifest['fonts']} != {16, 24}:
        raise RuntimeError('Both legacy fallback slots are required')
    for row in manifest['fonts']:
        slot = row['legacy_slot']
        code = (output_dir / row['file']).read_text()
        if hashlib.sha256(code.encode()).hexdigest() != row['sha256']:
            raise RuntimeError(f"{row['file']}: generated-file hash mismatch")
        actual = packed_codepoints(code)
        if actual != set(row['codepoints']):
            raise RuntimeError(f"{row['file']}: actual cmap differs from fallback manifest")
        fast = ui['ui_font_body' if slot == 16 else 'ui_font_title']
        if actual != old[slot] - fast or not old[slot] <= actual | fast:
            raise RuntimeError(f'{slot}: historical coverage or common-glyph deduplication failed')
        if not re.search(r'\.bpp\s*=\s*4', code) or '.bpp = 2' in code:
            raise RuntimeError('Expected native 4bpp data')
        if slot == 24:
            arrays = array_data(code)
            blob = bytes(numbers(arrays['passport_nickname_compressed'][1]))
            offsets = numbers(arrays['passport_nickname_block_offsets'][1])
            if offsets[0] != 0 or offsets[-1] != len(blob) or any(b <= a for a, b in zip(offsets, offsets[1:])):
                raise RuntimeError('Invalid block offset table/sentinel')
            blocks = [zlib.decompress(blob[a:b]) for a, b in zip(offsets, offsets[1:])]
            if row['block_size'] != BLOCK_SIZE or len(blocks) != row['block_count'] or any(
                    len(block) != min(BLOCK_SIZE, row['raw_size'] - i * BLOCK_SIZE)
                    for i, block in enumerate(blocks)):
                raise RuntimeError('Compressed font block geometry differs from decoder')
            raw = b''.join(blocks)
            if len(raw) != row['raw_size'] or hashlib.sha256(raw).hexdigest() != row['raw_sha256']:
                raise RuntimeError('Compressed font differs from its raw native 4bpp reference')
        print(f"{row['family']}: {row['native_size']}px, {len(actual)} rare glyphs; "
              f"legacy {len(old[slot])} covered with {len(fast)} fast glyphs")
    notice = ROOT / 'assets/fonts/FONT-LICENSES.txt'
    if notice.is_file():
        needed = {ord(c) for c in notice.read_text() if not c.isspace()}
        body = ui['ui_font_body'] | set(next(x['codepoints'] for x in manifest['fonts'] if x['legacy_slot'] == 16))
        missing = needed - body
        if missing:
            raise RuntimeError('License body missing: ' + ' '.join(f'U+{c:04X}' for c in sorted(missing)))
        print(f'Complete FONT-LICENSES text: {len(needed)} non-whitespace codepoints covered')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='check five-font UI and complete historical fallback coverage')
    parser.add_argument('--output-dir', type=Path, default=OUT_DIR, help='isolated candidate output directory')
    parser.add_argument('--reference-dir', type=Path, help='write native uncompressed fonts for host comparison')
    args = parser.parse_args()
    manifest_path = MANIFEST_PATH if args.output_dir.resolve() == OUT_DIR else args.output_dir / 'fallback-fonts.json'
    if args.check:
        check(args.output_dir, manifest_path)
        return
    source_check()
    ui, old = ui_fonts(), historical_sets()
    rare16, rare20 = sorted(old[16] - ui['ui_font_body']), sorted(old[24] - ui['ui_font_title'])
    if not rare16 or not rare20:
        raise RuntimeError('Unexpected empty fallback set')
    for size, chars in ((16, rare16), (20, rare20)):
        font = ImageFont.truetype(str(FONT_PATH), size)
        unknown = bytes(font.getmask(chr(0x10ffff)))
        for c in chars:
            mask = bytes(font.getmask(chr(c)))
            if mask == unknown or not any(mask):
                raise RuntimeError(f'Source lacks a visible glyph for U+{c:04X}')
    args.output_dir.mkdir(parents=True, exist_ok=True)
    if args.reference_dir:
        args.reference_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='passport-font-') as directory:
        temp = Path(directory)
        _, body = convert('passport_body_rare', 16, rare16, temp)
        body_code = font_header('Passport Body Fallback')
        body_code += '#if !LV_USE_FONT_COMPRESSED\n#error "Passport body fallback requires LVGL lossless decoding"\n#endif\n'
        body_code += emit_array('uint8_t', 'passport_body_glyph_bitmap', body['bitmap'])
        body_code += metadata(body, 'passport_body')
        body_code += 'static const lv_font_fmt_txt_dsc_t passport_body_dsc = {\n    ' + fmt_fields(body, 'passport_body', 'passport_body_glyph_bitmap', 1) + '\n};\n'
        body_code += font_struct('lv_font_cn_16', 'passport_body', body['line_height'], body['base_line'])
        parts = []
        for i in range(0, len(rare20), PART_GLYPHS):
            ref_name = f'passport_ref20_{i // PART_GLYPHS}'
            ref, part = convert(ref_name, 20, rare20[i:i + PART_GLYPHS], temp, raw=True)
            if max(g['bitmap_index'] for g in part['glyphs']) >= 1 << 20:
                raise RuntimeError('Part exceeds compact bitmap index; reduce PART_GLYPHS without deleting glyphs')
            parts.append(part)
            if args.reference_dir:
                (args.reference_dir / (ref_name + '.c')).write_text(ref)
        raw = b''.join(p['bitmap'] for p in parts)
        compressed, offsets = bytearray(), [0]
        for i in range(0, len(raw), BLOCK_SIZE):
            block = zlib.compress(raw[i:i + BLOCK_SIZE], 9)
            if zlib.decompress(block) != raw[i:i + BLOCK_SIZE]:
                raise RuntimeError('zlib block round-trip failed')
            compressed.extend(block)
            offsets.append(len(compressed))
        restored = b''.join(zlib.decompress(compressed[a:b]) for a, b in zip(offsets, offsets[1:]))
        base, compared = 0, 0
        for part in parts:
            for g in part['glyphs'][1:]:
                start, length = g['bitmap_index'], (g['box_w'] * g['box_h'] + 1) // 2
                if restored[base + start:base + start + length] != part['bitmap'][start:start + length]:
                    raise RuntimeError('Native 4bpp glyph round-trip mismatch')
                compared += 1
            base += len(part['bitmap'])
        code20 = font_header('Passport Nickname') + '#include "presentation/passport_font_blocks.h"\n'
        code20 += f'_Static_assert(PASSPORT_FONT_BLOCK_BYTES == {BLOCK_SIZE}u, "Regenerate font blocks for decoder format");\n'
        code20 += emit_array('uint8_t', 'passport_nickname_compressed', compressed)
        code20 += emit_array('uint32_t', 'passport_nickname_block_offsets', offsets)
        for i in range(1, len(parts)):
            code20 += f'static const lv_font_t passport_nickname_part_{i};\n'
        base, part_manifest = 0, []
        height, baseline = max(p['line_height'] for p in parts), max(p['base_line'] for p in parts)
        for i, part in enumerate(parts):
            prefix = f'passport_nickname_{i}'
            code20 += metadata(part, prefix)
            code20 += f'''static const passport_font_block_dsc_t {prefix}_dsc = {{
    .fmt = {{ {fmt_fields(part, prefix, 'NULL', 0)} }},
    .compressed = passport_nickname_compressed,
    .block_offsets = passport_nickname_block_offsets,
    .raw_size = {len(raw)}, .part_base = {base}, .block_count = {len(offsets) - 1},
    .compressed_size = {len(compressed)}, .glyph_count = {len(part['glyphs'])},
}};
'''
            next_font = f'&passport_nickname_part_{i + 1}' if i + 1 < len(parts) else 'NULL'
            symbol = 'lv_font_cn_24' if i == 0 else f'passport_nickname_part_{i}'
            code20 += font_struct(symbol, prefix, height, baseline, next_font, block=True, private=i > 0)
            part_manifest.append(dict(part=i, glyphs=len(part['glyphs']) - 1, glyph_count_with_reserved=len(part['glyphs']),
                                      part_base=base, raw_bytes=len(part['bitmap']), codepoints=part['codepoints']))
            base += len(part['bitmap'])
        if args.reference_dir:
            ref, _ = convert('passport_ref16', 16, rare16, temp, raw=True)
            (args.reference_dir / 'passport_ref16.c').write_text(ref)
        outputs = [('lv_font_cn_16.c', body_code), ('lv_font_cn_24.c', code20)]
        rows = []
        for slot, size, family, chars, (file, code), codec in zip(
                (16, 24), (16, 20), ('Passport Body Fallback', 'Passport Nickname'), (rare16, rare20),
                outputs, ('lvgl-lossless-rle', 'independent-zlib-blocks')):
            row = dict(legacy_slot=slot, native_size=size, family=family, file=file, codepoints=chars,
                       glyphs=len(chars), bpp=4, compression=codec, historical_codepoints=sorted(old[slot]),
                       historical_glyphs=len(old[slot]), fast_font='ui_font_body' if slot == 16 else 'ui_font_title',
                       sha256=hashlib.sha256(code.encode()).hexdigest())
            if slot == 24:
                row.update(block_size=BLOCK_SIZE, block_count=len(offsets) - 1, compressed_size=len(compressed),
                           raw_size=len(raw), raw_sha256=hashlib.sha256(raw).hexdigest(), parts=part_manifest,
                           glyphs_compared_to_native_4bpp=compared, line_height=height, base_line=baseline)
            rows.append(row)
        manifest = dict(format_version=1, scope='Only derivative font software; not the firmware application',
                        source=dict(family='LXGW WenKai Medium', version='1.522', path='assets/fonts/LXGWWenKai-Medium.ttf',
                                    sha256=SOURCE_SHA256, url='https://github.com/lxgw/LxgwWenKai/releases/download/v1.522/LXGWWenKai-Medium.ttf',
                                    license='SIL Open Font License 1.1', license_path='assets/fonts/LXGW-v1.522-OFL.txt'),
                        note='Legacy lv_font_cn_24 C symbol retained; actual native title/nickname size is 20px. '
                             'Both dynamic-text sizes cover full GB2312 Hanzi, retained name glyphs and dialogue extras. '
                             'No historical nickname glyphs were deleted; common glyphs are deduplicated.',
                        fast_font_glyphs={k: len(v) for k, v in ui.items()}, fonts=rows)
        for file, code in outputs:
            if packed_codepoints(code) != set(rare16 if file.endswith('_16.c') else rare20):
                raise RuntimeError('Emitted font cmap mismatch')
        for file, code in outputs:
            (args.output_dir / file).write_text(code)
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    check(args.output_dir, manifest_path)
    print(f'Compared {compared} native rare 20px glyph slices, including block crossings')


if __name__ == '__main__':
    main()
