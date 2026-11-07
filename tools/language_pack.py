"""Pack additional characters using the same bounded tiles as the Hangul fallback."""
import argparse,json,struct,sys,unicodedata,zlib
from pathlib import Path
from PIL import Image,ImageFont
ROOT=Path(__file__).resolve().parents[1]
HEADER=struct.Struct("<4sIHHhhHHI")

def coverage():
    source=json.loads((ROOT/"assets/fonts/language-repertoire.json").read_text())
    oracle=(ROOT/"tests/host/ui/font_oracle.bin").read_bytes()
    already={struct.unpack_from("<I",oracle,o)[0] for o in range(8,len(oracle),16)}
    chars=set(source["characters"])
    return sorted(ord(c) for c in chars if ord(c) not in already)

def tile_font(letters,px):
    face=ImageFont.truetype(str(ROOT/"assets/fonts/LXGWWenKai-Medium.ttf"),px,layout_engine=ImageFont.Layout.BASIC)
    masks=[]; advances=[]
    for cp in letters:
        text=chr(cp);mask,(x,y)=face.getmask2(text,mode="L",anchor="ls")
        image=Image.frombytes("L",mask.size,bytes(mask)).point(lambda p:255 if p>=96 else 0,mode="1")
        box=image.getbbox()
        if not box: raise ValueError(f"blank character U+{cp:04X}")
        masks.append((x+box[0],y+box[1],image.crop(box)))
        advances.append(round(face.getlength(text)))
    x0=min(x for x,y,m in masks);y0=min(y for x,y,m in masks)
    w=max(x+m.width for x,y,m in masks)-x0;h=max(y+m.height for x,y,m in masks)-y0
    step=512//(((w+7)//8)*h);tiles=[]
    for x,y,g in masks:
        im=Image.new("1",(w,h));im.paste(g,(x-x0,y-y0));tiles.append(im.tobytes())
    chunks=[]
    for i in range(0,len(tiles),step):
        c=zlib.compressobj(9,zlib.DEFLATED,-15);raw=b"".join(tiles[i:i+step])
        packed=c.compress(raw)+c.flush();assert zlib.decompress(packed,-15)==raw;chunks.append(packed)
    offsets=[HEADER.size+(len(chunks)+1)*4]
    for c in chunks:offsets.append(offsets[-1]+len(c))
    return bytes(advances)+HEADER.pack(b"KOR1",len(letters),w,h,x0,-(y0+h),step,px,len(chunks))+struct.pack("<"+"I"*len(offsets),*offsets)+b"".join(chunks)

def pack(letters):
    a=tile_font(letters,16)
    start=16+len(letters)*2
    return struct.pack("<4sIII",b"LNG1",len(letters),start,start+len(a))+struct.pack("<"+"H"*len(letters),*letters)+a

if __name__=="__main__":
    letters=coverage();data=pack(letters);p=ROOT/"assets/fonts/languages.bin"
    if "--check" in sys.argv:
        if p.read_bytes()!=data:raise SystemExit("stale language pack")
    else:p.write_bytes(data)
    print(f"{len(letters)} additional characters in a shared 16px atlas: {len(data)} bytes")

