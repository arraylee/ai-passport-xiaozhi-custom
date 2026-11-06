# AI PASSPORT font sources and derivatives

The source Chinese font is the unchanged LXGW WenKai Medium v1.522 TTF. Its
asset URL, version and SHA-256 are recorded in `font-sources.json`. The original
v1.522 license (`LXGW-v1.522-OFL.txt`) and separately preserved upstream-main
notice (`LXGW-OFL.txt`, with Reserved Font Names and additional permission) are
both kept verbatim. They are not represented as one unmodified upstream package.

The generated font families are **Passport UI**, **Passport Body Fallback**,
**Passport Nickname**, and **Passport Symbols and Latin**. Original font names
are source attribution, not the primary names of the derivatives or a claim of
endorsement. Existing C symbol names remain internal compatibility identifiers.

`FONT-LICENSES.txt` contains complete original notices for WenKai/Klee,
Montserrat and Font Awesome 5, with source and version attribution. The original
Montserrat and FontAwesome5 notices are copied from the actual LVGL dependency.
OFL applies to font software and derived glyph data only, not to the rest of the
firmware. All notices are copied into the flat firmware ZIP and embedded as the
same UTF-8 text in the standalone application BIN. Long-press Confirm on the home screen to open the offline license
reader, then use normal-key previous/next/return controls.

## Reproducible generation

1. `python3 tools/gen_font_notices.py` creates the notice text, provenance JSON
   and Flash C string from the original notices.
2. `python3 tools/gen_ui_fonts.py` creates native 4bpp 14/16/20/24/32px UI subsets.
3. `python3 tools/gen_cn_font.py` creates compatible rare-character fallbacks,
   deduplicating the fast UI subsets. Both 16px and 20px cover all 6,763 GB2312
   Hanzi, retained name glyphs, punctuation and colloquial additions. Together
   with fast UI subsets each size currently resolves 7,035 codepoints. The
   legacy `lv_font_cn_24` slot is native 20px, matching
   the actual title/nickname size; its full repertoire shares a 22px line height.
4. `python3 tools/gen_font_oracle.py` records independent uncompressed 4bpp
   pixel checksums for all 14,070 size/codepoint combinations. This fixture is
   used only by host tests and is never included in the application.
5. Each command supports `--check`. Run all four checks before release.

Common UI glyphs and the rare 16px fallback use LVGL lossless compression. The
rare 20px fallback uses independent 512-byte zlib blocks in Flash and the same
bounded puff decoder on the host and ESP32-C3. It uses at most 512 bytes of
temporary system heap plus stack-local inflater state and the existing LVGL A8
glyph buffer. The 16px fallback uses LVGL RLE and two short scanline buffers.
The LVGL pool remains 40KiB. No full-font RAM buffer or permanent cache is used.

Since .10, `tools/body_font_pack.py` externalizes 16px bitmaps, native compact
metrics and Unicode maps into a BDF1 resource at offset 0x10000 inside the
existing readonly `petart` partition (absolute address 0x510000). The original
PTP1 sprite bytes remain unchanged at 0x500000; all partition boundaries and
device identity/configuration regions are unchanged. CPR1 v3 contains the
matching BDF1 digest, the 20px font and unchanged portrait frames. Firmware
checks exact headers, compile-time CRCs and readonly mapping before binding.
Release tools and the web flasher check both fingerprints, sizes and hashes.

`font_mapped_all_glyphs` executes the production resource loader and the exact
externalized font C sources. Every glyph's metrics and decoded pixels must
match the independently generated native oracle. Missing/corrupt packs,
mapping failures, failed-init cleanup and repeated init are also checked.

Device acceptance must still measure heap and render latency during audio
activity. A host font/pool test is not device evidence. Coverage is finite:
7,035 codepoints do not mean every Unicode character or emoji is included.
