#!/usr/bin/env python3
"""Generate native 4bpp WenKai subsets from the actual C UI string literals."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import subprocess
from PIL import ImageFont

ROOT = Path(__file__).resolve().parents[1]
FONT = ROOT / "assets/fonts/LXGWWenKai-Medium.ttf"
OUT = ROOT / "components/ui/presentation/src"
TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"')


def strings(path):
    for match in TOKEN.finditer(path.read_text()):
        value = match.group()
        if value.startswith('"'):
            # C source text is UTF-8; remove C escapes without re-decoding Chinese bytes.
            yield re.sub(r'\\(?:x[0-9a-fA-F]+|[0-7]{1,3}|.)', '', value[1:-1])


def subsets():
    files = [p for p in OUT.glob("*.c") if p.name not in ("ui_pet.c", "ui_bobo.c") and not p.name.startswith(("lv_font", "ui_font", "bobo_art"))]
    files += [ROOT / "components/app/src/app_plays.c"]
    # Protocol/intent tokens are never rendered as built-in UI copy.
    files += [p for p in (ROOT / "components/platform/xiaozhi").glob("*.c") if p.name != "xz_protocol.c"]
    text = "".join(value for p in files for value in strings(p))
    # Common punctuation appears in cloud replies even when absent from UI copy.
    text += "，。！？；：、…“”‘’（）【】《》〈〉「」『』—·～"
    text += (ROOT / "assets/fonts/FONT-LICENSES.txt").read_text()
    answers = (ROOT / "assets/source/answers.txt").read_text()
    ascii_chars = set(map(chr, range(32, 127)))
    ui = sorted({c for c in text + answers if ord(c) >= 32 and not c.isspace()} | ascii_chars)
    answer = sorted({c for c in answers if ord(c) >= 32 and not c.isspace()} | ascii_chars)
    return [("ui_font_body", 16, ui), ("ui_font_title", 20, ui), ("ui_font_caption", 14, ui),
            ("ui_font_answer", 24, answer), ("ui_font_book", 32, sorted(set("答案之书")))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--uncompressed", action="store_true", help="generate an uncompressed reference for pixel comparison")
    parser.add_argument("--output-dir", type=Path, help="write a comparison font set outside the firmware source tree")
    args = parser.parse_args()
    output_dir = args.output_dir or OUT
    output_dir.mkdir(parents=True, exist_ok=True)
    manifest = {"family": "Passport UI", "source_family": "LXGW WenKai Medium", "source_version": "1.522",
                "license": "SIL OFL 1.1 (font data only)", "notices": "assets/fonts/FONT-LICENSES.txt",
                "bpp": 4, "compression": "none" if args.uncompressed else "lossless-rle",
                "source_sha256": hashlib.sha256(FONT.read_bytes()).hexdigest(), "fonts": []}
    for name, size, chars in subsets():
        destination = output_dir / (name + ".c")
        if args.check:
            code = destination.read_text()
            actual = {int(x, 16) for x in re.findall(r'UI_CMAP U\+([0-9A-F]+)', code)}
            missing = set(map(ord, chars)) - actual
            if missing:
                raise SystemExit(f"{name}: missing {''.join(map(chr, sorted(missing)))}")
            if not re.search(r'\.bpp\s*=\s*4', code):
                raise SystemExit(f"{name}: expected genuine 4bpp glyphs")
            if not args.uncompressed and not re.search(r'\.bitmap_format\s*=\s*[12]', code):
                raise SystemExit(f"{name}: expected lossless compressed glyphs")
        else:
            probe = ImageFont.truetype(str(FONT), size)
            unknown = bytes(probe.getmask(chr(0x10FFFF)))
            for char in chars:
                if not char.isspace() and bytes(probe.getmask(char)) == unknown:
                    raise SystemExit(f"Source font lacks U+{ord(char):04X}")
            subprocess.run(["npx", "--yes", "lv_font_conv@1.5.3", "--font", str(FONT),
                "--size", str(size), "--bpp", "4", *(["--no-compress"] if args.uncompressed else []), "--no-kerning",
                "--format", "lvgl", "--lv-include", "lvgl.h", "--symbols", "".join(chars),
                "--output", str(destination)], check=True)
            code = destination.read_text().replace(str(ROOT) + "/", "")
            if not args.uncompressed:
                code = code.replace('#include "lvgl.h"', '#include "lvgl.h"\n#if !LV_USE_FONT_COMPRESSED\n#error "AI PASSPORT fonts require lossless font decoding"\n#endif')
            if name in ("ui_font_body", "ui_font_title", "ui_font_caption"):
                fallback = "lv_font_cn_24" if name == "ui_font_title" else "lv_font_cn_16"
                code = code.replace('#include "lvgl.h"', '#include "lvgl.h"\nLV_FONT_DECLARE(' + fallback + ');')
                code = code.replace('.fallback = NULL', '.fallback = &' + fallback)
            if name == "ui_font_title":
                # Match the full native20 nickname repertoire, including its
                # tallest supplementary-plane glyph; never scale the bitmap.
                code = re.sub(r'\.line_height\s*=\s*\d+', '.line_height = 22', code)
                code = re.sub(r'\.base_line\s*=\s*\d+', '.base_line = 4', code)
            attribution = ('/* Passport UI: native bitmap derivative; source attribution only:\n'
                ' * LXGW WenKai Medium v1.522. Copyright 2021-2026 LXGW\n'
                ' * (https://github.com/lxgw/LxgwWenKai).\n'
                ' * Copyright 2020 The Klee Project Authors\n'
                ' * (https://github.com/fontworks-fonts/Klee).\n'
                ' * This font data is distributed under SIL OFL 1.1.\n'
                ' * Full notices: assets/fonts/FONT-LICENSES.txt; embedded in passport_font_notices.\n'
                ' * OFL applies to font data, not the rest of the firmware.\n'
                ' */\n')
            destination.write_text(attribution + "\n".join(f"/* UI_CMAP U+{ord(c):04X} */" for c in chars) + "\n" + code)
        manifest["fonts"].append({"name": name, "size": size, "glyphs": len(chars),
                                  "codepoints": [ord(c) for c in chars]})
        print(f"{name}: {size}px, genuine 4bpp, {len(chars)} codepoints")
    if not args.check:
        manifest_path = output_dir / "ui-fonts.json" if args.output_dir else ROOT / "assets/fonts/ui-fonts.json"
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
