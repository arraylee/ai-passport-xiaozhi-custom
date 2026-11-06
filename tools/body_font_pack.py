"""BDF1: native 16px glyphs/metrics in the unused tail of readonly petart.

The existing PTP1 sprites and all partition boundaries stay unchanged. The font
starts at a 64KiB page boundary and is mapped, never copied into a RAM cache.
"""
import hashlib
import re
import struct
import zlib
try:
    from .gen_cn_font import array_data, numbers
except ImportError:
    from gen_cn_font import array_data, numbers

OFFSET = 0x10000
CAPACITY = 0x100000
HEADER = 64


def fingerprint(blob):
    return 'BODYFONT-SHA256:' + hashlib.sha256(blob).hexdigest()


def validate(blob):
    if not HEADER <= len(blob) <= CAPACITY - OFFSET:
        raise ValueError('body font length')
    h = struct.unpack_from('<4s15I', blob)
    if h[:3] != (b'BDF1', 1, len(blob)) or h[3] != zlib.crc32(blob[16:]):
        raise ValueError('body font header/CRC')
    bitmap, size, metrics, count, maps = h[4:9]
    if not 1 <= maps <= 2 or any(h[9 + maps * 2:]):
        raise ValueError('body font cmap count/reserved')
    entries = [(bitmap, size), (metrics, count * 8)]
    entries += [(h[9 + i * 2], h[10 + i * 2] * 2) for i in range(maps)]
    end = HEADER
    for at, length in entries:
        if at != ((end + 3) & ~3) or length <= 0 or at + length > len(blob):
            raise ValueError('body font bounds/alignment')
        if any(x != 255 for x in blob[end:at]):
            raise ValueError('body font padding')
        end = at + length
    if end != len(blob) or not 2 <= count <= 65535:
        raise ValueError('body font trailing bytes/count')
    if sum(h[10 + i * 2] for i in range(maps)) != count - 1:
        raise ValueError('body font cmap/glyph count')
    if blob[metrics:metrics + 8] != bytes(8):
        raise ValueError('body font reserved glyph')
    last_index = -1
    for n in range(1, count):
        word, w, height, _, _ = struct.unpack_from('<I4B', blob, metrics + n * 8)
        index = word & 0xfffff
        if not last_index <= index < size or not 1 <= w <= 32 or not 1 <= height <= 32:
            raise ValueError('body font glyph bounds')
        last_index = index
    for at, length in entries[2:]:
        points = struct.unpack_from('<' + 'H' * (length // 2), blob, at)
        if any(b <= a for a, b in zip(points, points[1:])):
            raise ValueError('body font cmap order')
    return h


def build(source, out):
    arrays = array_data(source)
    bitmap_name = 'passport_body_glyph_bitmap'
    metrics_name = 'passport_body_glyph_dsc'
    unicode_names = [name for name in arrays if name.startswith('passport_body_unicode_list_')]
    if not 1 <= len(unicode_names) <= 2:
        raise ValueError('expected sparse body cmaps')
    blob = bytearray(HEADER)
    sections = []
    data = [bytes(numbers(arrays[bitmap_name][1]))]
    metrics = bytearray()
    for row in re.findall(r'\{([^{}]*)\}', arrays[metrics_name][1]):
        g = {k: int(v) for k, v in re.findall(r'\.(\w+)\s*=\s*(-?\d+)', row)}
        if not 0 <= g['bitmap_index'] < 1 << 20 or not 0 <= g['adv_w'] < 1 << 12:
            raise ValueError('glyph exceeds compact LVGL ABI')
        metrics += struct.pack('<I4B', g['bitmap_index'] | (g['adv_w'] << 20),
                               g['box_w'], g['box_h'], g['ofs_x'] & 255, g['ofs_y'] & 255)
    data.append(bytes(metrics))
    for name in unicode_names:
        values = numbers(arrays[name][1])
        data.append(struct.pack('<' + 'H' * len(values), *values))
    for raw in data:
        blob.extend(b'\xff' * (-len(blob) % 4))
        sections.append((len(blob), len(raw)))
        blob.extend(raw)
    h = [b'BDF1', 1, len(blob), 0, *sections[0], sections[1][0], len(metrics) // 8, len(unicode_names)]
    for at, length in sections[2:]:
        h += [at, length // 2]
    h += [0] * (16 - len(h))
    struct.pack_into('<4s15I', blob, 0, *h)
    struct.pack_into('<I', blob, 12, zlib.crc32(blob[16:]))
    validate(blob)
    for name in [bitmap_name, metrics_name, *unicode_names]:
        source = re.sub(r'static const \w+ ' + name + r'\[\]\s*=\s*\{.*?\n\};', '', source, flags=re.S)
        source = re.sub(r'= ' + name + r'(?=,)', '= NULL', source)
    source = source.replace('static const lv_font_fmt_txt_dsc_t', 'static lv_font_fmt_txt_dsc_t')
    source = source.replace('static const lv_font_fmt_txt_cmap_t', 'static lv_font_fmt_txt_cmap_t')
    guard = '''
_Static_assert(sizeof(lv_font_fmt_txt_glyph_dsc_t) == 8 && !LV_FONT_FMT_TXT_LARGE,
               "BDF1 needs compact LVGL glyph metrics");
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "BDF1 needs little-endian native metrics"
#endif
static bool passport_body_lookup(const lv_font_t *f, lv_font_glyph_dsc_t *g, uint32_t c, uint32_t next) {
    return passport_body_dsc.glyph_dsc && lv_font_get_glyph_dsc_fmt_txt(f, g, c, next);
}
static const void *passport_body_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *b) {
    return passport_body_dsc.glyph_bitmap ? lv_font_get_bitmap_fmt_txt(g, b) : NULL;
}
'''
    source = source.replace('const lv_font_t lv_font_cn_16', guard + '\nconst lv_font_t lv_font_cn_16')
    source = source.replace('.get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt', '.get_glyph_dsc = passport_body_lookup')
    source = source.replace('.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt', '.get_glyph_bitmap = passport_body_bitmap')
    source += '\nvoid passport_bind_body(const uint8_t *data) {\n'
    source += f'    passport_body_dsc.glyph_bitmap = data ? data + {sections[0][0]} : NULL;\n'
    source += f'    passport_body_dsc.glyph_dsc = data ? (const void *)(data + {sections[1][0]}) : NULL;\n'
    for i, (at, _) in enumerate(sections[2:]):
        source += f'    passport_body_cmaps[{i}].unicode_list = data ? (const void *)(data + {at}) : NULL;\n'
    source += '}\n'
    (out / 'body_font.bin').write_bytes(blob)
    (out / 'body_font_meta.h').write_text(
        '#pragma once\n' + f'#define BODY_FONT_OFFSET {OFFSET}u\n#define BODY_FONT_SIZE {len(blob)}u\n'
        + f'#define BODY_FONT_CRC 0x{zlib.crc32(blob[16:]):08x}u\n'
        + '#define BODY_FONT_HEADER_BYTES {' + ','.join(map(str, blob[:HEADER])) + '}\n'
        + f'#define BODY_FONT_FINGERPRINT "{fingerprint(blob)}"\n')
    return bytes(blob), source


def bundle(sprites, catalog, font):
    validate(font)
    if len(sprites) > OFFSET:
        raise ValueError('sprites overlap the fixed body font area')
    combined = sprites + b'\xff' * (OFFSET - len(sprites)) + font
    catalog = dict(catalog, body_font={'offset_in_partition': OFFSET, 'bytes': len(font),
                                     'sha256': hashlib.sha256(font).hexdigest(), 'format': 'BDF1'})
    return combined, catalog


def split(data, catalog):
    """Validate both old sprite-only packages and paired .10 font bundles."""
    meta = catalog.get('body_font')
    if meta is None:
        return data, None
    length = catalog.get('bytes', 0)
    if not 64 <= length <= OFFSET or any(x != 255 for x in data[length:OFFSET]):
        raise ValueError('body font bundle padding')
    font = data[OFFSET:]
    validate(font)
    expected = {'offset_in_partition': OFFSET, 'bytes': len(font),
                'sha256': hashlib.sha256(font).hexdigest(), 'format': 'BDF1'}
    if meta != expected:
        raise ValueError('body font catalog mismatch')
    return data[:length], font
