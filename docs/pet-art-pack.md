# Native sprite resource delivery

Native PSR1 sprites are stored in `petart.bin`, in the new read-only `petart`
partition at `0x500000` with capacity `0x100000`. Every existing partition keeps
its original offset and size. The partition ends at `0x600000`, within 8 MiB
Flash. The identity range `0x356000..0x359fff` remains untouched.

`tools/build_pixel_assets.py` supplies `(symbol, image, PSR1 bytes)` records to
`tools/pet_art_pack.py::write_pet_art_assets`. The helper emits mutable LVGL
source descriptors and a constant catalog, preserving every C image symbol.
Pixel payloads are absent from application rodata. `bobo_art_asset_count()` and
`bobo_art_asset(index)` enumerate the existing constant catalog in stable ID
order, returning ID/dimensions/CRC/descriptor metadata without extra per-asset
RAM or name strings; an out-of-range index returns NULL. The existing native PSR1
row decoder is unchanged: one decoded row is allocated per open image, with no
whole-image cache. The descriptors and catalog are the only per-asset RAM/Flash
metadata. The 1 MiB resource capacity is separate from the 3 MiB app partition.

## On-disk PTP1 version 1

All integers use little endian. Header size is 64 bytes:

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | `PTP1` magic |
| 4 | 2 | Version 1 |
| 6 | 2 | Header size 64 |
| 8 | 4 | Exact total byte length |
| 12 | 4 | Asset count, 1..1024 |
| 16 | 4 | Index entry size 24 |
| 20 | 4 | Index offset 64 |
| 24 | 4 | Payload offset, exactly 64 + count × 24 |
| 28 | 4 | IEEE CRC-32 of the complete index |
| 32 | 4 | IEEE CRC-32 of the complete payload |
| 36 | 28 | Zero reserved bytes |

Each index entry contains: FNV-1a 32-bit symbol ID, width u16, height u16,
absolute payload offset u32, exact length u32, payload CRC-32 u32, and zero u32.
Entries are ordered by strictly increasing ID; duplicate symbols or hash
collisions fail generation. Payloads are contiguous, without gaps or trailing
bytes. Dimensions must fit 240×320, and every PSR1 row, RLE run, row offset and
palette index must be valid. CRCs detect accidental corruption; this is not a
signed resource format and makes no authenticity claim.

## Startup and failure behavior

`platform_pet_art_map` verifies the locked partition address, capacity,
read-only flag and declared pack length, then maps only the necessary Flash
pages for the firmware lifetime. It never writes/erases the partition or copies
the whole pack to heap. Application startup invokes `bobo_art_bind` before
creating any UI and outside the LVGL lock.

Binding first clears all descriptors. It validates the full pack against the
compiled catalog's exact count, IDs, dimensions, sizes and per-asset CRCs,
including every PSR1 row. Only after all validation passes does a second pass
expose image data pointers. `bobo_art_ready()` reports success. Missing,
truncated, corrupted, wrong-version or mismatched packs leave all data pointers
NULL, so the sprite decoder safely rejects rendering. The UI can offer a
return action with an unavailable-resource message. A failed rebind also clears
a previous valid binding; callers must stop all image/decoder users before any
rebind or unbind. Normal firmware binds once at boot.

The generated application retains `PETART-SHA256:<hex>` for the exact resource
pack. Release validation checks this fingerprint against both the pack and its
catalog, preventing an application from being packaged with a stale art pack.
Host tests use a dedicated file loader before `main`; only that host loader
allocates the whole pack, and it does not access a device. The pixel decoder
test automatically enumerates every catalog entry, including newly added pets
and gift frames. `tests/host/pixel_sprite/prepare_references.py` resolves each
symbol through `pixel-assets.json` to its native PNG, checks exact set/dimension/
payload-metadata agreement, and writes ID-named RGBA fixtures. `bobo_art_` maps
to the unprefixed PNG name, `dodge_art_` maps to `dodge_`, and other symbols keep
their names. Both host CMake suites generate these references automatically and
set `PIXEL_SPRITE_REFERENCE_DIR` for `ctest`, so every normal test run includes
full-catalog pixel-exact RGBA and premultiplied-alpha checks. Direct binary runs
can set the same environment variable to a prepared reference directory.

## Release contract

The split package contains five explicit segments: bootloader `0x0`, partition
table `0x8000`, app `0x10000`, avatars `0x3fa000`, and pet art `0x500000`.
The pet pack file uses its exact byte length; tooling verifies its sector-rounded
4 KiB erase extent stays inside `0x500000..0x5fffff`. All existing NVS, user
images, audio, and identity partitions are outside every segment's erase
extent. The ZIP includes the pack catalog, per-file SHA-256 sums, exact write
arguments, font notices and a machine-readable manifest.

The app-only BIN is written at `0x10000` and requires its matching `petart.bin`.
It is not a complete Web BIN. A single raw merged image spanning the protected
identity gap must never be produced. Web tooling must understand all five
explicit segments; otherwise use the split package. Building, host rendering,
and packaging are separate from unperformed physical-device acceptance.
