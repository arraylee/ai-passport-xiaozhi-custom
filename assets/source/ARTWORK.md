# Firmware artwork sources

These are design assets, not photographs of the device. Only source files used
by the current generator are included; rejected studies and old previews remain
outside the committed source set.

The product owner's four toy turnarounds are in `companions/references/`.
Sunflower retains its brown face, red lips, large eyes and pot; cactus its cowboy
hat, red scarf, vest, belt and pot; panda its black eye patches; Lele its brown
bear body and red ribbed scarf. Lele is distinct from the human-faced Bobo.

The companion and Bobo action sheets were generated with the built-in image tool
using the authorized character references. Three growth stages and four poses
are converted once to native 120/80-pixel canvases. Bobo baby idle/sleep and
wave/happy use the v6 corrections; junior/adult use v5. Each pose has two hands.
The eight gift frames share a fixed canvas and base, so opening the lid does not
resize the box. Inventory icons are native 32×32, obstacles 48×64, small walking
frames 40×40. No runtime bitmap scaling is used.

The eight original demo avatars in `assets/avatars/` are unchanged 96×156 PNGs,
129,032 bytes total. The generator does not resize or overwrite them. Backgrounds
and Answer Book decorations use LVGL geometry instead of full-screen bitmaps.

`tools/build_pixel_assets.py` reproducibly converts the retained source sheets,
removes the extraction matte, and emits native PNGs, PSR1 data and the PTP1 pack.
`assets/pixel-assets.json` records input hashes and output dimensions/hashes.
The 144 sprites have 508,336 bytes of payload; `petart.bin` is 511,856 bytes with
its index/header. Host tests compare every sprite to its native PNG, including
alpha and RGB565 output. Firmware decodes one row, not a complete image cache.

`answers.txt` retains the product owner's original text corpus. Font subsets and
the text-based Book of Answers use this corpus rather than answer photographs.
