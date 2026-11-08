#!/usr/bin/env python3
"""Create the merged playable BIN downloaded by FoloToy Recovery protocol 4."""
import argparse
import hashlib
import json
from pathlib import Path
import struct

from compact_faces import STRIPS
from companion_pack import validate as validate_companion

SECTOR = 4096
APP_OFFSET = 0x10000
JOURNAL = 0x6b8000
MAX_DOWNLOAD = 6_000_000  # Also fits users/clients that interpret MB as decimal.
PROTECTED = {"netcfg": (1, 2, 0x6bc000, 0x4000),
             "recovery": (0, 0x20, 0x6c0000, 0x140000)}


def check(ok, message):
    if not ok:
        raise ValueError(message)


def partitions(table):
    check(len(table) == 0xc00, "Incomplete partition table")
    parts = {}
    last_end = 0x9000
    for at in range(0, len(table), 32):
        tag, kind, subtype, start, size, label, flags = struct.unpack_from("<HBBII16sI", table, at)
        if tag == 0xebeb:
            check(table[at+16:at+32] == hashlib.md5(table[:at]).digest(), "Partition table MD5")
            check(all(parts.get(k) == v for k, v in PROTECTED.items()), "Recovery partitions changed")
            return parts
        name = label.split(b"\0")[0].decode("ascii")
        check(tag == 0x50aa and flags == 0 and kind in (0, 1) and name and name not in parts,
              "Unsupported partition entry or flags")
        check(size and not (start % SECTOR or size % SECTOR) and start >= last_end,
              "Unaligned or overlapping partition")
        check(name in PROTECTED or start + size <= JOURNAL, "Overlaps Recovery workspace")
        parts[name] = kind, subtype, start, size
        last_end = start + size
    raise ValueError("Missing partition MD5")


def verify_esp(blob):
    """Check the ESP32-C3 image's segment bounds, XOR checksum and appended SHA."""
    check(len(blob) > 56 and blob[0] == 0xe9 and blob[12:14] == b'\x05\x00' and blob[23] == 1,
          "Expected hash-appended ESP32-C3 image")
    pos, checksum = 24, 0xef
    for _ in range(blob[1]):
        check(pos + 8 <= len(blob), "Truncated ESP segment")
        length = int.from_bytes(blob[pos+4:pos+8], "little")
        pos += 8
        check(pos + length <= len(blob), "Truncated ESP payload")
        for byte in memoryview(blob)[pos:pos+length]:
            checksum ^= byte
        pos += length
    end = (pos + 16) & ~15
    check(end + 32 == len(blob) and blob[end-1] == checksum, "ESP image checksum")
    check(hashlib.sha256(blob[:end]).digest() == blob[end:], "ESP image SHA-256")


def build(directory):
    flash = json.loads((directory / "flasher_args.json").read_text())
    table = (directory / flash["partition-table"]["file"]).read_bytes()
    parts = partitions(table)
    expected = {"bootloader":0, "partition-table":0x8000, "app":APP_OFFSET,
                **{name:parts[name][2] for name in ("petart", "companion", "imgava")}}
    check({int(k, 0) for k in flash["flash_files"]} == set(expected.values()), "Unexpected flash inputs")
    check(parts["factory"][:3] == (0,0,APP_OFFSET) and parts["factory"][3] <= 0x600000,
          "Invalid factory application slot")
    image = bytearray()
    payloads = {}
    for name, start in sorted(expected.items(), key=lambda item:item[1]):
        entry = flash[name]
        check(int(entry["offset"], 0) == start and flash["flash_files"][hex(start)] == entry["file"],
              "Manifest disagreement for " + name)
        data = (directory / entry["file"]).read_bytes()
        if name in ("app", "bootloader"):
            verify_esp(data)
        if name == "bootloader":
            check(b"UP held: booting permanent recovery" in data, "Missing permanent Recovery hook")
        if name == "imgava":
            end = int.from_bytes(data[8:12], "little")
            check(data[:4] == b"AVA1" and 16 <= end <= len(data) and data[end:] == b'\xff'*(len(data)-end),
                  "Invalid avatar length or padding")
            data = data[:end]
        partition_name = "factory" if name == "app" else name
        if partition_name in parts:
            check(len(data) <= parts[partition_name][3], name + " exceeds its partition")
        check(len(image) <= start, "Overlapping inputs")
        image.extend(b'\xff' * (start - len(image)))
        image.extend(data)
        payloads[name] = data
    faces = payloads["companion"]
    entries = validate_companion(faces)
    check(int.from_bytes(faces[4:8], "little") == 4, "Expected compact face pack")
    for at, size in entries[2:]:
        frame = faces[at:at+size]
        offsets = struct.unpack_from(f"<{STRIPS+1}H", frame, 4)
        check(offsets[0] == 4 + 2*(STRIPS+1) and offsets[-1] == len(frame) and
              all(x < y for x,y in zip(offsets, offsets[1:])), "Invalid face strip index")
    face_sha = hashlib.sha256(faces).hexdigest()
    check(("COMPANION-SHA256:" + face_sha).encode() in payloads["app"], "Mismatched face pack")
    body = (directory / "companion/body_font.bin").read_bytes()
    check(payloads["petart"][0x10000:] == body and
          ("BODYFONT-SHA256:" + hashlib.sha256(body).hexdigest()).encode() in payloads["app"],
          "Mismatched body font")
    check(len(image) < MAX_DOWNLOAD, "Mini-program download exceeds the size goal")
    return image


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build-miniapp"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    image = build(args.build_dir)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"{len(image)} bytes, SHA-256 {hashlib.sha256(image).hexdigest()}  {args.output.name}")
