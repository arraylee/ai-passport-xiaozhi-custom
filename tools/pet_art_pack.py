#!/usr/bin/env python3
"""PTP1 immutable native sprite pack and generated LVGL binding catalog.

The pack is canonical by stable FNV-1a symbol ID. CRCs detect accidental damage;
matching per-asset metadata plus the embedded SHA-256 release fingerprint keeps
application and resource deliveries paired. This is not a signed trust format.
No generated sprite payload is compiled into the application.
"""
import argparse
import hashlib
import json
import re
import struct
import zlib
from pathlib import Path

CAPACITY = 0x100000
HEADER = struct.Struct('<4sHH7I28s')
ENTRY = struct.Struct('<IHHIIII')
MAX_COUNT = 1024
# Retained UI resources only: no pet poses, gifts, icons, or growth frames.
SHARED_GAME_SYMBOLS = frozenset({
    'bobo_art_wave', 'bobo_art_walk_left', 'bobo_art_walk_right',
    'dodge_art_books', 'dodge_art_luggage', 'dodge_art_robot', 'dodge_art_plant',
})


def asset_id(symbol):
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', symbol):
        raise ValueError('invalid sprite C symbol')
    value = 2166136261
    for byte in symbol.encode('ascii'):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def fingerprint(data):
    return 'PETART-SHA256:' + hashlib.sha256(data).hexdigest()


def validate_sprite(data, width, height):
    if not (1 <= width <= 240 and 1 <= height <= 320):
        raise ValueError('sprite dimensions exceed device bounds')
    if len(data) < 8 or data[:4] != b'PSR1' or not 1 <= data[4] <= 64 or data[5:8] != b'\0'*3:
        raise ValueError('invalid PSR1 header')
    table = 8 + data[4] * 4
    base = table + (height+1)*2
    if not base < len(data) <= base + 65535:
        raise ValueError('invalid PSR1 stream bounds')
    offsets = struct.unpack_from('<'+'H'*(height+1), data, table)
    if offsets[0] or offsets[-1] != len(data)-base:
        raise ValueError('invalid PSR1 offsets')
    for begin, end in zip(offsets, offsets[1:]):
        if not 0 <= begin < end <= len(data)-base:
            raise ValueError('invalid PSR1 row bounds')
        pos, expanded = base+begin, 0
        while pos < base+end:
            tag = data[pos]; pos += 1
            count = (tag & 127)+1
            size = 1 if tag & 128 else count
            if expanded+count > width or pos+size > base+end or any(x >= data[4] for x in data[pos:pos+size]):
                raise ValueError('invalid PSR1 row run or palette index')
            pos += size; expanded += count
        if expanded != width:
            raise ValueError('PSR1 row width mismatch')


def build_pack(entries):
    entries = list(entries)
    if not 1 <= len(entries) <= MAX_COUNT:
        raise ValueError('invalid sprite count')
    entries.sort(key=lambda item: asset_id(item[0]))
    ids, index, payload, records = set(), bytearray(), bytearray(), []
    base = HEADER.size + len(entries)*ENTRY.size
    for symbol, width, height, data in entries:
        ident = asset_id(symbol)
        if ident in ids: raise ValueError('duplicate sprite symbol or ID collision')
        ids.add(ident)
        validate_sprite(data, width, height)
        offset = base + len(payload)
        crc = zlib.crc32(data)
        index.extend(ENTRY.pack(ident, width, height, offset, len(data), crc, 0))
        records.append(dict(symbol=symbol, asset_id=ident, width=width, height=height,
                            offset=offset, bytes=len(data), crc32=crc,
                            sha256=hashlib.sha256(data).hexdigest()))
        payload.extend(data)
    total = base + len(payload)
    if total > CAPACITY: raise ValueError('pet art exceeds 1 MiB partition capacity')
    data = HEADER.pack(b'PTP1', 1, HEADER.size, total, len(entries), ENTRY.size,
                       HEADER.size, base, zlib.crc32(index), zlib.crc32(payload), b'\0'*28) + index + payload
    catalog = dict(format='PTP1', version=1, offset=0x500000, capacity=CAPACITY,
                   bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                   count=len(records), sprite_bytes=len(payload), sprites=records)
    return bytes(data), catalog


def validate_pack(data, catalog=None):
    if not HEADER.size <= len(data) <= CAPACITY: raise ValueError('invalid pack length')
    magic, version, header, total, count, size, index, base, index_crc, data_crc, reserved = HEADER.unpack_from(data)
    if (magic, version, header, size, index, reserved) != (b'PTP1', 1, HEADER.size, ENTRY.size, HEADER.size, b'\0'*28):
        raise ValueError('invalid pack format or reserved fields')
    if total != len(data) or not 1 <= count <= MAX_COUNT or base != index + count*size or base >= total:
        raise ValueError('invalid pack count or bounds')
    if zlib.crc32(data[index:base]) != index_crc or zlib.crc32(data[base:]) != data_crc:
        raise ValueError('pack CRC mismatch')
    records, last_id, end = [], -1, base
    for pos in range(index, base, size):
        ident, width, height, offset, length, crc, reserved = ENTRY.unpack_from(data, pos)
        if ident <= last_id or reserved or offset != end or length > total-offset or not length:
            raise ValueError('invalid asset ID, order, offset or bounds')
        raw = data[offset:offset+length]
        if zlib.crc32(raw) != crc: raise ValueError('asset CRC mismatch')
        validate_sprite(raw, width, height)
        records.append(dict(asset_id=ident, width=width, height=height, offset=offset,
                            bytes=length, crc32=crc, sha256=hashlib.sha256(raw).hexdigest()))
        last_id, end = ident, offset+length
    if end != total: raise ValueError('unindexed trailing bytes')
    if catalog is not None:
        if not isinstance(catalog, dict) or not isinstance(catalog.get("sprites"), list):
            raise ValueError("invalid catalog structure")
        if (catalog.get('format') != 'PTP1' or catalog.get('version') != 1 or
                catalog.get('offset') != 0x500000 or catalog.get('capacity') not in (CAPACITY, 0xe9000) or len(data) > catalog['capacity'] or
                catalog.get('bytes') != len(data) or catalog.get('count') != count or
                catalog.get('sha256') != hashlib.sha256(data).hexdigest() or
                len(catalog.get('sprites', [])) != count):
            raise ValueError('catalog and pack mismatch')
        for actual, expected in zip(records, catalog['sprites']):
            if not isinstance(expected, dict) or not isinstance(expected.get('symbol'), str):
                raise ValueError('invalid catalog asset')
            if any(actual[key] != expected.get(key) for key in actual) or asset_id(expected.get('symbol', '')) != actual['asset_id']:
                raise ValueError('catalog asset mismatch')
            actual['symbol'] = expected['symbol']
    return records


def shared_game_pack(data, catalog):
    """Validate the complete source before keeping every required shared asset."""
    records = validate_pack(data, catalog)
    kept = [r for r in records if r['symbol'] in SHARED_GAME_SYMBOLS]
    missing = SHARED_GAME_SYMBOLS - {r['symbol'] for r in kept}
    if missing:
        raise ValueError('missing shared sprite assets: ' + ', '.join(sorted(missing)))
    return build_pack((r['symbol'], r['width'], r['height'],
                       data[r['offset']:r['offset']+r['bytes']]) for r in kept)


def validate_shared_game_catalog(catalog):
    if {r['symbol'] for r in catalog['sprites']} != SHARED_GAME_SYMBOLS:
        raise ValueError('shared game art must contain only all seven home-avatar/Dodge sprites')


def write_pet_art_assets(root, entries):
    """Emit C/header/pack/catalog from [(C symbol, PIL image, PSR1 bytes)]."""
    root = Path(root)
    data, catalog = build_pack((symbol, image.width, image.height, raw) for symbol, image, raw in entries)
    header = ['/* Generated by tools/pet_art_pack.py; do not edit. */', '#pragma once',
              '#include "presentation/pet_art_pack.h"', '#include <stddef.h>', '#include <stdbool.h>',
              '/* Bind once before UI creation. Source mapping must outlive the UI.',
              ' * A failed bind clears every descriptor and sets ready=false.',
              ' * Rebinding/unbinding requires all users/decoder sessions to stop. */',
              'bool bobo_art_bind(const void *data, size_t size);',
              'void bobo_art_unbind(void);', 'bool bobo_art_ready(void);',
              'extern const char bobo_art_pack_fingerprint[];',
              '/* Stable ID order; metadata is available even before binding. */',
              'size_t bobo_art_asset_count(void);',
              'const pet_art_asset_t *bobo_art_asset(size_t index);']
    code = ['/* Generated native descriptors; PTP1 bytes stay in mapped Flash. */',
            '#include "presentation/bobo_art.h"', '#include "presentation/pet_art_pack.h"',
            '#if LV_BIN_DECODER_RAM_LOAD', '#error "Native sprites require row decoding"', '#endif',
            f'const char bobo_art_pack_fingerprint[] = "{fingerprint(data)}";', 'static bool ready;']
    for record in catalog['sprites']:
        symbol = record['symbol']
        header.append(f'extern lv_image_dsc_t {symbol};')
        code.append(f'lv_image_dsc_t {symbol} = {{ .header = {{ .magic = LV_IMAGE_HEADER_MAGIC, '
                    '.cf = LV_COLOR_FORMAT_RAW_ALPHA, .flags = LV_IMAGE_FLAGS_USER1, '
                    f'.w = {record["width"]}, .h = {record["height"]} }} }};')
    code.append('static const pet_art_asset_t catalog[] = {')
    for r in catalog['sprites']:
        code.append(f'    {{ 0x{r["asset_id"]:08x}u, {r["width"]}, {r["height"]}, '
                    f'{r["bytes"]}u, 0x{r["crc32"]:08x}u, &{r["symbol"]} }},')
    code += ['};', '#define ASSET_COUNT (sizeof(catalog) / sizeof(catalog[0]))',
             'bool bobo_art_bind(const void *data, size_t size) {',
             '    ready = pet_art_pack_bind(data, size, catalog, ASSET_COUNT, bobo_art_pack_fingerprint);',
             '    return ready;', '}',
             'void bobo_art_unbind(void) { pet_art_pack_unbind(catalog, ASSET_COUNT); ready = false; }',
             'bool bobo_art_ready(void) { return ready; }',
             'size_t bobo_art_asset_count(void) { return ASSET_COUNT; }',
             'const pet_art_asset_t *bobo_art_asset(size_t index) {',
             '    return index < ASSET_COUNT ? &catalog[index] : NULL;', '}']
    outputs = {'assets/petart.bin': data,
               'assets/petart_catalog.json': (json.dumps(catalog, indent=2, sort_keys=True)+'\n').encode(),
               'components/ui/presentation/include/presentation/bobo_art.h': ('\n'.join(header)+'\n').encode(),
               'components/ui/presentation/src/bobo_art.c': ('\n'.join(code)+'\n').encode()}
    for relative, raw in outputs.items():
        path = root/relative; path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists() or path.read_bytes() != raw: path.write_bytes(raw)
    return catalog


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--catalog', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--catalog-output', type=Path, required=True)
    parser.add_argument('--size', type=lambda value: int(value, 0), required=True)
    parser.add_argument('--body-font', type=Path, help='BDF1 font in unused readonly space at +64KiB')
    args = parser.parse_args()
    if args.size not in (CAPACITY, 0xe9000): parser.error('unexpected petart capacity')
    raw = args.input.read_bytes(); catalog = json.loads(args.catalog.read_text())
    validate_pack(raw, catalog)
    validate_shared_game_catalog(catalog)
    if args.body_font:
        import body_font_pack
        raw, catalog = body_font_pack.bundle(raw, catalog, args.body_font.read_bytes())
    # Exact-length pack: only its sector-rounded extent is erased/written.
    if len(raw) > args.size: parser.error('font/sprites exceed the readonly partition')
    catalog['capacity'] = args.size
    args.output.write_bytes(raw)
    args.catalog_output.write_text(json.dumps(catalog, indent=2, sort_keys=True)+'\n')
    print(f'PTP1: {catalog["count"]} sprites, {len(raw)}/{args.size} bytes')


if __name__ == '__main__': main()
