#!/usr/bin/env python3
"""Export the two generated transparent sheets into aligned native face frames.

This is an offline asset conversion, never run on the device. Component bounds
remove uneven sheet gutters without repainting the generated face or its alpha.
"""
from collections import deque
import json
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1] / 'assets/xiaozhi'


def head_bounds(image):
    width, height = image.size
    mask = bytearray(value > 128 for value in image.getchannel('A').tobytes())
    parts = []
    for start in range(width * height):
        if not mask[start]:
            continue
        queue = deque([start])
        mask[start] = 0
        count = 0
        left = right = start % width
        top = bottom = start // width
        while queue:
            pos = queue.popleft()
            x, y = pos % width, pos // width
            count += 1
            left, right = min(left, x), max(right, x)
            top, bottom = min(top, y), max(bottom, y)
            neighbors = []
            if x: neighbors.append(pos - 1)
            if x < width - 1: neighbors.append(pos + 1)
            if y: neighbors.append(pos - width)
            if y < height - 1: neighbors.append(pos + width)
            for neighbor in neighbors:
                if mask[neighbor]:
                    mask[neighbor] = 0
                    queue.append(neighbor)
        if count > 20000:
            parts.append((left, top, right + 1, bottom + 1))
    if len(parts) != 6:
        raise ValueError(f'Expected six separate heads, found {len(parts)}')
    return sorted(parts, key=lambda b: (b[1] // (height // 2), b[0]))


def main():
    manifest = []
    (ROOT / 'faces').mkdir(exist_ok=True)
    for avatar, name in enumerate(('girl', 'boy')):
        source = f'source/{name}-faces.png'
        image = Image.open(ROOT / source).convert('RGBA')
        width, height = image.size
        for frame, (left, top, right, bottom) in enumerate(head_bounds(image)):
            crop = (max(0, left - 3), max(0, top - 3), min(width, right + 3), min(height, bottom + 3))
            tile = image.crop(crop)
            tile.thumbnail((138, 138), Image.Resampling.LANCZOS)
            native = Image.new('RGBA', (144, 144))
            native.alpha_composite(tile, ((144 - tile.width) // 2, 141 - tile.height))
            file = f'face-{avatar:02}-{frame:02}.png'
            native.save(ROOT / 'faces' / file)
            manifest.append(dict(avatar=avatar, frame=frame, file=file,
                                 source=source, crop=crop, native=[144, 144]))
    (ROOT / 'faces.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    main()
