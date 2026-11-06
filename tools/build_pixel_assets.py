#!/usr/bin/env python3
"""Deterministically crop generated artwork into native firmware pixel assets.

Creative source images stay untouched. This step only removes the documented
matte, extracts fixed sheet cells, reduces a palette and encodes target pixels.
"""
from pathlib import Path
import hashlib
import json
import struct
from PIL import Image, ImageDraw
from pet_art_pack import write_pet_art_assets, SHARED_GAME_SYMBOLS

ROOT = Path(__file__).resolve().parents[1]


def cells(path, matte=False, columns=4, rows=2, row_edges=None, matte_min=120,
          matte_green_ratio=.65, trim=True):
    sheet = Image.open(path).convert("RGBA")
    if matte:
        data = []
        for r, g, b, a in sheet.getdata():
            if (r > matte_min and b > matte_min - 10 and b > r * .65
                    and g < r * matte_green_ratio and g < b * matte_green_ratio):
                a = 0
            data.append((r, g, b, a))
        sheet.putdata(data)
    result = []
    edges = row_edges or [round(i * sheet.height / rows) for i in range(rows + 1)]
    if len(edges) != rows + 1 or edges[0] != 0 or edges[-1] != sheet.height:
        raise ValueError("Sheet row boundaries do not match its dimensions")
    for row in range(rows):
        for col in range(columns):
            image = sheet.crop((round(col * sheet.width / columns), edges[row],
                                round((col + 1) * sheet.width / columns), edges[row + 1]))
            mask = image.getchannel("A").point(lambda a: 255 if a >= 128 else 0)
            image.putalpha(mask)
            box = mask.getbbox()
            if box is None:
                raise ValueError("Empty sheet cell")
            result.append(image.crop(box) if trim else image)
    return result


def native_art(image, width, height, margin):
    # Render once at the actual device size. No coarse intermediate grid or
    # integer enlargement; retain soft edge alpha and 64 color/alpha entries.
    image = image.copy()
    image.thumbnail((width - margin * 2, height - margin * 2), Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (width, height))
    canvas.alpha_composite(image, ((width - image.width) // 2, height - margin - image.height))
    return canvas.quantize(colors=64, method=Image.Quantize.FASTOCTREE,
                           dither=Image.Dither.NONE).convert("RGBA")


def encode_sprite_rle(image):
    palette, lookup, indexes = [], {}, []
    for rgba in image.getdata():
        rgba = rgba if rgba[3] else (0, 0, 0, 0)
        if rgba not in lookup:
            if len(palette) >= 64:
                raise ValueError("PSR1 supports at most 64 colors")
            lookup[rgba] = len(palette)
            r, g, b, a = rgba
            palette.append(bytes((b, g, r, a)))
        indexes.append(lookup[rgba])
    stream, offsets = bytearray(), [0]
    for y in range(image.height):
        row = indexes[y * image.width:(y + 1) * image.width]
        x = 0
        while x < len(row):
            count = 1
            while x + count < len(row) and row[x + count] == row[x] and count < 128:
                count += 1
            if count >= 3:
                stream.extend((0x80 | (count - 1), row[x])); x += count
            else:
                start = x; x += count
                while x < len(row) and x - start < 128:
                    run = 1
                    while x + run < len(row) and row[x + run] == row[x] and run < 3:
                        run += 1
                    if run >= 3:
                        break
                    x += min(run, 128 - (x - start))
                stream.append(x - start - 1); stream.extend(row[start:x])
        offsets.append(len(stream))
    if offsets[-1] > 65535:
        raise ValueError("PSR1 stream is too large")
    return b"PSR1" + bytes((len(palette), 0, 0, 0)) + b"".join(palette) + struct.pack(
        "<" + "H" * len(offsets), *offsets) + stream


def main():
    assets = ROOT / "assets/bobo"
    assets.mkdir(parents=True, exist_ok=True)
    bobo_source = ROOT / "assets/source/bobo/bobo-actions-source.png"
    obstacle_source = ROOT / "assets/source/dodge/obstacles-source.png"
    poses = cells(bobo_source, matte=True)
    names = [("wave", 1), ("walk_left", 2), ("walk_right", 3)]
    manifest = {"sources": {}, "sprites": [], "avatars": []}
    for path in (bobo_source, obstacle_source):
        manifest["sources"][str(path.relative_to(ROOT))] = hashlib.sha256(path.read_bytes()).hexdigest()
    rendered = []
    sprite_images = [("bobo_art_" + name, native_art(poses[index],
                      40 if name.startswith("walk") else 80,
                      40 if name.startswith("walk") else 80, 2)) for name, index in names]
    obstacle_names = ("luggage", "robot", "plant", "books")
    sprite_images += [("dodge_art_" + name, native_art(source, 48, 64, 2))
                     for name, source in zip(obstacle_names, cells(obstacle_source, matte=True, columns=2))]
    packed = []
    for symbol, image in sprite_images:
        if symbol not in SHARED_GAME_SYMBOLS:
            continue
        name = symbol.replace("bobo_art_", "").replace("dodge_art_", "dodge_")
        output = assets / (name + ".png")
        image.save(output, optimize=True)
        rendered.append(image)
        data = encode_sprite_rle(image)
        packed.append((symbol, image, data))
        manifest["sprites"].append({"id": name, "width": image.width, "height": image.height,
                                    "format": "PSR1-64", "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
    write_pet_art_assets(ROOT, packed)
    portraits = []
    # The user restored the demo's exact native PNGs. They are source assets,
    # never resampled or regenerated when Bobo artwork is rebuilt.
    for i in range(1, 9):
        output = ROOT / f"assets/avatars/avatar-{i:02}.png"
        portrait = Image.open(output).convert("RGBA")
        if portrait.size != (96, 156):
            raise ValueError("Demo avatars must retain their native 96x156 size")
        manifest["sources"][str(output.relative_to(ROOT))] = hashlib.sha256(output.read_bytes()).hexdigest()
        portraits.append(portrait)
        manifest["avatars"].append({"id": output.stem, "width": 96, "height": 156,
                                    "bytes": output.stat().st_size, "sha256": hashlib.sha256(output.read_bytes()).hexdigest()})
    manifest["sprite_flash_bytes"] = sum(item["bytes"] for item in manifest["sprites"])
    (ROOT / "assets/pixel-assets.json").write_text(json.dumps(manifest, indent=2) + "\n")
    # Native-size QA board; the upper row is exactly the firmware 80px sprites.
    board = Image.new("RGB", (840, 300), "#17221e")
    draw = ImageDraw.Draw(board)
    for i, picture in enumerate(rendered[:len(names)]):
        x = 12 + i * 102
        board.paste(picture, (x + (80 - picture.width) // 2, 12), picture)
        draw.text((x, 94), names[i][0], fill="#efede4")
    for i, picture in enumerate(portraits):
        board.paste(picture, (10 + i * 103, 128), picture)
    evidence = ROOT / "docs/evidence"
    evidence.mkdir(parents=True, exist_ok=True)
    board.save(evidence / "pixel-assets-native.png")
    obstacles = Image.new("RGB", (260, 94), "#17221e")
    for i, picture in enumerate(rendered[len(names):len(names)+4]):
        obstacles.paste(picture, (8 + i * 64, 8), picture)
    obstacles.save(evidence / "dodge-assets-native.png")

    print(f"{len(sprite_images)} sprites: {manifest['sprite_flash_bytes']} Flash bytes; avatars: {sum(x['bytes'] for x in manifest['avatars'])} PNG bytes")


if __name__ == "__main__":
    main()
