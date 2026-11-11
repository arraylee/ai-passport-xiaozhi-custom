#!/usr/bin/env python3
"""Build immutable mmap resources; keep generated font sources as host oracles."""
import argparse, hashlib, json, re, struct, zlib
from pathlib import Path
from PIL import Image
try:
    from .gen_cn_font import array_data, numbers
    from . import body_font_pack, compact_faces
except ImportError:
    from gen_cn_font import array_data, numbers
    import body_font_pack, compact_faces
ROOT = Path(__file__).resolve().parents[1]
SIZE = 0x200000
HEADER = 128
AVATARS = 2
FRAMES = 6
WIDTH = HEIGHT = 144
FRAME_BYTES = WIDTH * HEIGHT * 3

def validate(blob):
    if len(blob) < HEADER or len(blob) > SIZE or blob[:4] != b'CPR1': raise ValueError('resource size/magic')
    version, total, crc = struct.unpack_from('<III', blob, 4)
    if version not in (1, 2, 3, 4) or not HEADER <= total <= len(blob) or (version != 4 and len(blob) != SIZE): raise ValueError('resource version/length')
    if zlib.crc32(blob[16:total]) != crc: raise ValueError('resource CRC')
    end = HEADER
    entries = []
    count, frame_size = (4, 96*156*3) if version == 1 else (2+AVATARS*FRAMES, FRAME_BYTES)
    for i in range(count):
        offset, length = struct.unpack_from('<II', blob, 16 + i*8)
        if offset < end or offset % 4 or length == 0 or offset + length > total: raise ValueError('resource range')
        if version == 4 and i >= 2 and blob[offset:offset+4] != b'XZF1': raise ValueError('packed face header')
        if version != 4 and i >= 2 and length != frame_size: raise ValueError('avatar dimensions')
        if version >= 3 and i == 0 and length != 32: raise ValueError('external body font digest')
        entries.append((offset, length)); end=offset+length
    return entries

def build(out, miniapp=False):
    out.mkdir(parents=True, exist_ok=True)
    blob=bytearray(HEADER);entries=[]
    for file,symbol,bind in [('lv_font_cn_16.c','passport_body_glyph_bitmap','passport_bind_body'),('lv_font_cn_24.c','passport_nickname_compressed','passport_bind_nickname')]:
        source=(ROOT/'components/ui/presentation/src'/file).read_text()
        if bind.endswith('body'):
            body, source = body_font_pack.build(source, out, 0x4000 if miniapp else body_font_pack.OFFSET)
            data = hashlib.sha256(body).digest()
        else:
            data=bytes(numbers(array_data(source)[symbol][1]))
        start=len(blob);blob.extend(data)
        entries.append((start,len(data)))
        blob.extend(b'\xff'*(-len(blob)%4))
        if bind.endswith('body'):
            (out/file).write_text(source)
            continue
        source=re.sub(r'static const uint8_t '+symbol+r'\[\]\s*=\s*\{.*?\};','',source,flags=re.S)
        source=source.replace('static const lv_font_fmt_txt_dsc_t passport_body_dsc','static lv_font_fmt_txt_dsc_t passport_body_dsc')
        source=source.replace('static const passport_font_block_dsc_t','static passport_font_block_dsc_t')
        source=source.replace('= '+symbol+',','= NULL,')
        names=re.findall(r'static (?:lv_font_fmt_txt_dsc_t|passport_font_block_dsc_t) (\w+)_dsc =',source)
        field='glyph_bitmap' if bind.endswith('body') else 'compressed'
        if bind.endswith('body'):
            source=source.replace('const lv_font_t lv_font_cn_16', 'static const void *passport_body_bitmap(lv_font_glyph_dsc_t *g, lv_draw_buf_t *b) {\n    return passport_body_dsc.glyph_bitmap ? lv_font_get_bitmap_fmt_txt(g,b) : NULL;\n}\nconst lv_font_t lv_font_cn_16')
            source=source.replace('.get_glyph_bitmap = lv_font_get_bitmap_fmt_txt', '.get_glyph_bitmap = passport_body_bitmap')
        source+='\nvoid '+bind+'(const uint8_t *data) {\n'+''.join('    '+n+'_dsc.'+field+' = data;\n' for n in names)+'}\n'
        (out/file).write_text(source)
    for image in sorted((ROOT/'assets/xiaozhi/faces').glob('face-*.png')):
        im=Image.open(image).convert('RGBA')
        if im.size!=(WIDTH,HEIGHT): raise ValueError(f'non-native avatar {image}: {im.size}')
        pixels=list(im.getdata()); rgb=bytearray();alpha=bytearray()
        for r,g,b,a in pixels:
            rgb.extend(struct.pack('<H',((r>>3)<<11)|((g>>2)<<5)|(b>>3)));alpha.append(a)
        data = compact_faces.pack_face(rgb + alpha) if miniapp else rgb + alpha
        entries.append((len(blob),len(data)));blob.extend(data);blob.extend(b'\xff'*(-len(blob)%4))
    if len(entries)!=2+AVATARS*FRAMES: raise ValueError('expected two fonts and twelve expression frames')
    korean_at = len(blob)
    blob.extend((ROOT/'assets/fonts/hangul20.bin').read_bytes())
    blob.extend(b'\xff' * (-len(blob)%4))
    icons_at = len(blob)
    for name in ('neutral','happy','thinking','sleepy','sad','surprised'):
        icon=Image.open(ROOT/f'assets/xiaozhi/official/{name}.png').convert('RGBA').resize((32,32),Image.Resampling.LANCZOS)
        pixels=list(icon.getdata())
        blob.extend(b''.join(struct.pack('<H',((r>>3)<<11)|((g>>2)<<5)|(b>>3)) for r,g,b,a in pixels))
        blob.extend(bytes(a for r,g,b,a in pixels))
    default_avatar_at=len(blob)
    blob.extend(Image.open(ROOT/'assets/avatars/avatar-01.png').convert('RGBA').tobytes('raw','BGRA'))
    blob[:4]=b'CPR1';struct.pack_into('<II',blob,4,4 if miniapp else 3,len(blob))
    for i,e in enumerate(entries):struct.pack_into('<II',blob,16+i*8,*e)
    struct.pack_into('<I',blob,12,zlib.crc32(blob[16:]))
    digest=hashlib.sha256(blob).hexdigest(); total=len(blob)
    if not miniapp:
        blob.extend(b'\xff'*(SIZE-len(blob)))
    validate(blob)
    (out/'companion.bin').write_bytes(blob)
    (out/'companion_pack_meta.h').write_text(f'#pragma once\n#define OFFICIAL_ICONS_OFFSET {icons_at}u\n#define DEFAULT_AVATAR_OFFSET {default_avatar_at}u\n#define HANGUL_TITLE_BYTES {len((ROOT/"assets/fonts/hangul20.bin").read_bytes())}u\n#define HANGUL_TITLE_OFFSET {korean_at}u\n#define COMPANION_SIZE {total}u\n#define COMPANION_CRC 0x{struct.unpack_from("<I",blob,12)[0]:08x}u\n#define COMPANION_SHA "{digest}"\n')
    (out/'companion_catalog.json').write_text(json.dumps(dict(format='CPR1',version=4 if miniapp else 3,portraits=AVATARS,frames_per_portrait=FRAMES,width=WIDTH,height=HEIGHT,used=total,size=len(blob),sha256=hashlib.sha256(blob).hexdigest(),entries=entries,body_font_bytes=len(body)),indent=2)+'\n')
    # Only device-mapped fonts gain an extra fallback; the source oracle stays independent.
    for file, size in [('lv_font_cn_16.c', 16), ('lv_font_cn_24.c', 20)]:
        path = out / file
        source = path.read_text()
        assert source.count('.fallback = NULL') == 1
        source = source.replace('.fallback = NULL', f'.fallback = &passport_ko_{size}')
        path.write_text('#include "presentation/hangul_font.h"\n' + source)
    print(f'Companion resources: {total}/{SIZE} bytes; fonts {entries[0][1]+entries[1][1]}, two animated faces {AVATARS*FRAMES*FRAME_BYTES}')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--miniapp',action='store_true')
    args=p.parse_args();build(args.output,args.miniapp)
