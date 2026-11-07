"""Generate the two Flash-resident Hangul fonts from the project's licensed TTF."""
import argparse
import hashlib
import struct
import zlib
from pathlib import Path
from PIL import Image, ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "assets/fonts/LXGWWenKai-Medium.ttf"
LETTERS = list(range(0xAC00, 0xD7A4)) + list(range(0x3131, 0x318F))
HEADER = struct.Struct("<4sIHHhhHHI")

def make_font(px):
    if hashlib.sha256(FONT.read_bytes()).hexdigest() != "d4bdeb38a39151d74d084cba5090f8cb7d20bf83eedb78c35939ae70b9f4e3f6":
        raise ValueError("unexpected source font")
    face = ImageFont.truetype(str(FONT), px, layout_engine=ImageFont.Layout.BASIC)
    # Use actual ink bounds to avoid storing invisible advance-width margins.
    glyphs = []
    for cp in LETTERS:
        mask, (x, y) = face.getmask2(chr(cp), mode="L", anchor="ls")
        gray = Image.frombytes("L", mask.size, bytes(mask))
        ink = gray.point(lambda p: 255 if p >= 96 else 0, mode="1")
        box = ink.getbbox()
        if not box and cp != 0x3164:
            raise ValueError(f"blank Korean glyph U+{cp:04X}")
        if box:
            glyphs.append((x + box[0], y + box[1], ink.crop(box)))
        else:
            glyphs.append((0, 0, None))
    x0 = min(x for x,y,g in glyphs if g)
    y0 = min(y for x,y,g in glyphs if g)
    w = max(x + g.width for x,y,g in glyphs if g) - x0
    h = max(y + g.height for x,y,g in glyphs if g) - y0
    group = 512 // (((w+7)//8)*h)
    raw = []
    for x,y,g in glyphs:
        tile = Image.new("1", (w,h))
        if g: tile.paste(g, (x-x0,y-y0))
        raw.append(tile.tobytes())
    blocks = []
    for i in range(0,len(raw),group):
        codec = zlib.compressobj(9, zlib.DEFLATED, -15)
        data = b"".join(raw[i:i+group])
        compressed = codec.compress(data) + codec.flush()
        assert zlib.decompress(compressed, -15) == data
        blocks.append(compressed)
    start = HEADER.size + (len(blocks)+1)*4
    offsets = [start]
    for block in blocks: offsets.append(offsets[-1]+len(block))
    return HEADER.pack(b"KOR1",len(raw),w,h,x0,-(y0+h),group,px,len(blocks)) + struct.pack("<"+"I"*len(offsets),*offsets) + b"".join(blocks)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args=parser.parse_args()
    for size in (16,20):
        packed=make_font(size)
        target=ROOT/f"assets/fonts/hangul{size}.bin"
        if args.check:
            if target.read_bytes()!=packed: raise SystemExit(f"stale {target}")
        else:
            target.write_bytes(packed)
        print(f"{size}px: {len(LETTERS)} glyphs, {len(packed)} bytes")

