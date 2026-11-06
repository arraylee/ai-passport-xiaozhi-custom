"""Resolve every catalog symbol through the PNG manifest into ID-keyed RGBA fixtures."""
from pathlib import Path
import argparse
import json
import sys
from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.pet_art_pack import validate_pack


def png_name(symbol):
    if symbol.startswith("bobo_art_"):
        return symbol[len("bobo_art_"):]
    if symbol.startswith("dodge_art_"):
        return "dodge_" + symbol[len("dodge_art_"):]
    return symbol


def prepare_references(assets, output):
    assets, output = Path(assets), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    sprite_list = json.loads((assets.parent / "pixel-assets.json").read_text())["sprites"]
    sprites = {item["id"]: item for item in sprite_list}
    if len(sprites) != len(sprite_list):
        raise ValueError("duplicate PNG manifest IDs")
    catalog = json.loads((assets.parent / "petart_catalog.json").read_text())
    records = validate_pack((assets.parent / "petart.bin").read_bytes(), catalog)
    if len(records) != len(sprites):
        raise ValueError("PNG manifest and pack catalog counts differ")
    references, used_names = [], set()
    for asset in records:
        name = png_name(asset["symbol"])
        if name in used_names or name not in sprites:
            raise ValueError(f"catalog symbol has no unique PNG manifest entry: {asset['symbol']}")
        used_names.add(name)
        sprite = sprites[name]
        if (sprite["format"] != "PSR1-64" or
                any(sprite[key] != asset[key] for key in ("width", "height", "bytes", "sha256"))):
            raise ValueError(f"PNG manifest and pack metadata differ: {name}")
        with Image.open(assets / f"{name}.png") as source:
            image = source.convert("RGBA")
        if image.size != (asset["width"], asset["height"]):
            raise ValueError(f"PNG dimensions differ: {name}: {image.size}")
        filename = f"{asset['asset_id']:08x}.rgba"
        (output / filename).write_bytes(image.tobytes())
        references.append(dict(asset_id=asset["asset_id"], symbol=asset["symbol"], png=name+".png",
                               rgba=filename, width=asset["width"], height=asset["height"]))
    if used_names != set(sprites):
        raise ValueError("PNG manifest contains assets missing from compiled catalog")
    (output / "references.json").write_text(json.dumps(references, indent=2)+"\n")
    return references


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("assets", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    references = prepare_references(args.assets, args.output)
    print(f"Prepared all {len(references)} catalog sprites as ID-keyed native RGBA fixtures.")


if __name__ == "__main__": main()
