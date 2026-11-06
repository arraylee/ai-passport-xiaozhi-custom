#!/usr/bin/env python3
"""Verify a six-segment AI PASSPORT release ZIP and optional build inputs."""

import argparse
import hashlib
import json
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from scripts import release  # noqa: E402


EXPECTED_SEGMENTS = (
    ("bootloader", "bootloader.bin", 0x000000, 0x000000, 0x008000),
    ("partition-table", "partition-table.bin", 0x008000, 0x008000, 0x009000),
    ("factory", "trae_card.bin", 0x010000, 0x010000, 0x310000),
    ("imgava", "imgava.bin", 0x3FA000, 0x3FA000, 0x4FA000),
    ("petart", "petart.bin", 0x500000, 0x500000, 0x600000),
    ("companion", "companion.bin", 0x600000, 0x600000, 0x800000),
)
REQUIRED_METADATA = {
    "avatar_catalog.json", "petart_catalog.json", "flash_args", "flasher_args.json",
    "firmware-manifest.json", "MANIFEST.txt", "SHA256SUMS",
} | set(release.FONT_METADATA_FILES) | set(release.LIBRARY_LICENSE_FILES)


def _digest(data):
    return hashlib.sha256(data).hexdigest()


def _parse_sums(data):
    result = {}
    for line in data.decode("ascii").splitlines():
        try:
            digest, name = line.split("  ", 1)
        except ValueError as exc:
            raise RuntimeError(f"SHA256SUMS 行格式错误: {line!r}") from exc
        if name in result or len(digest) != 64:
            raise RuntimeError(f"SHA256SUMS 条目无效或重复: {name}")
        result[name] = digest
    return result


def verify_archive(archive_path, build_dir=None, standalone_app=None):
    archive_path = Path(archive_path)
    if not archive_path.is_file():
        raise RuntimeError(f"release ZIP 不存在: {archive_path}")
    with zipfile.ZipFile(archive_path) as package:
        names = package.namelist()
        if len(names) != len(set(names)):
            raise RuntimeError("release ZIP 含重复路径")
        if any(Path(name).name != name or name.endswith("/") for name in names):
            raise RuntimeError("release ZIP 必须只含根目录扁平文件")
        required = {item[1] for item in EXPECTED_SEGMENTS} | REQUIRED_METADATA
        missing = required - set(names)
        if missing:
            raise RuntimeError(f"release ZIP 缺少: {', '.join(sorted(missing))}")
        if "merged-firmware.bin" in names:
            raise RuntimeError("release ZIP 不允许跨分区 raw merge")
        payload = {name: package.read(name) for name in names}

    try:
        manifest = json.loads(payload["firmware-manifest.json"])
        flat_args = json.loads(payload["flasher_args.json"])
    except (ValueError, KeyError) as exc:
        raise RuntimeError(f"release JSON 无效: {exc}") from exc
    if manifest.get("package_type") != "explicit-flash-segments":
        raise RuntimeError("firmware-manifest.json package_type 错误")
    release.validate_font_license_payload(payload, payload["trae_card.bin"])
    release.validate_library_license_payload(payload)
    for key, notice in (("welcome_audio", "WELCOME-SOURCE.txt"), ("farewell_audio", "FAREWELL-SOURCE.txt"), ("startup_audio", "BOOT-SOURCE.txt")):
        if key not in manifest:
            continue
        entry = manifest[key]
        allowed_formats = [("IMA-ADPCM", 16000)]
        if key == "welcome_audio": allowed_formats.append(("PCM-S16LE", 24000))
        if (not isinstance(entry, dict) or (entry.get("format"), entry.get("sample_rate")) not in allowed_formats
                or entry.get("notice") != notice or not payload.get(notice)):
            raise RuntimeError(f"{key} 语音格式或来源说明无效")
        # Releases before .5 did not record the offset of their farewell.
        if "app_offset" not in entry and key == "farewell_audio":
            continue
        offset, size = entry.get("app_offset"), entry.get("bytes")
        app = payload["trae_card.bin"]
        if (type(offset) is not int or type(size) is not int or offset < 0 or size <= 0
                or offset + size > len(app) or _digest(app[offset:offset+size]) != entry.get("sha256")):
            raise RuntimeError(f"{key} 与应用镜像内语音不匹配")
        if entry["format"] == "PCM-S16LE" and size % 2:
            raise RuntimeError(f"{key} PCM 样本长度无效")
    notices = manifest.get("font_notices", {})
    if (notices.get("file") != "FONT-LICENSES.txt"
            or notices.get("sha256") != _digest(payload["FONT-LICENSES.txt"])
            or notices.get("embedded_in_application") is not True):
        raise RuntimeError("manifest 字体许可嵌入声明错误")
    segments = manifest.get("segments")
    if not isinstance(segments, list) or len(segments) != 6:
        raise RuntimeError("firmware-manifest.json 必须含六个写入段")

    release.validate_companion_payload(payload["companion.bin"], payload["trae_card.bin"])
    pet_catalog = release.validate_pet_art_payload(
        payload["petart.bin"], payload["petart_catalog.json"], payload["trae_card.bin"])
    shared = "shared_game_art" in manifest
    if shared:
        if manifest.get("features") != {"pet": False} or "pet_art" in manifest:
            raise RuntimeError("shared game art must declare pet removed without legacy pet metadata")
        try:
            release.pet_art_pack.validate_shared_game_catalog(pet_catalog)
        except ValueError as exc:
            raise RuntimeError(str(exc)) from exc
        if manifest["shared_game_art"].get("uses") != ["dodge", "home-avatar"]:
            raise RuntimeError("shared game art uses mismatch")
    elif manifest.get("features", {}).get("pet") is False:
        raise RuntimeError("pet-free firmware requires shared_game_art metadata")
    pet_meta = manifest.get("shared_game_art" if shared else "pet_art", {})
    expected_body = (dict(pet_catalog['body_font'], file='petart.bin', offset=0x510000)
                     if 'body_font' in pet_catalog else None)
    if manifest.get('body_font') != expected_body:
        raise RuntimeError('manifest body font metadata mismatch')
    if (pet_meta.get("file") != "petart.bin" or pet_meta.get("offset") != 0x500000
            or pet_meta.get("capacity") != 0x100000 or pet_meta.get("format") != "PTP1"
            or pet_meta.get("count") != pet_catalog["count"]
            or pet_meta.get("bytes") != len(payload["petart.bin"])
            or pet_meta.get("sha256") != _digest(payload["petart.bin"])):
        raise RuntimeError("manifest petart metadata mismatch")
    standalone = manifest.get("standalone_application", {})
    matching_key = "requires_matching_shared_game_art" if shared else "requires_matching_pet_art"
    if shared and "requires_matching_pet_art" in standalone:
        raise RuntimeError("pet-free standalone app must use shared game art requirement")
    if standalone.get(matching_key) != {
            "file": "petart.bin", "offset": 0x500000, "sha256": _digest(payload["petart.bin"])}:
        raise RuntimeError("standalone app must declare matching petart.bin requirement")
    if standalone.get("requires_matching_companion") != {"file": "companion.bin", "offset": 0x600000, "sha256": _digest(payload["companion.bin"])}:
        raise RuntimeError("standalone app must declare matching companion.bin")
    if standalone.get("complete_flash_image") is not False:
        raise RuntimeError("standalone app is not a complete flash image")
    expected_flash = {}
    for item, expected in zip(segments, EXPECTED_SEGMENTS):
        name, file_name, offset, region_start, region_end = expected
        if (item.get("name"), item.get("file"), item.get("offset")) != (
                name, file_name, offset):
            raise RuntimeError(f"manifest 写入段不匹配: expected {expected[:3]}")
        data = payload[file_name]
        if item.get("size") != len(data) or item.get("sha256") != _digest(data):
            raise RuntimeError(f"manifest 的 {file_name} size/SHA-256 不匹配")
        erase_start, erase_end = release.validate_write_extent(
            f"{name} {file_name}", offset, len(data), region_start, region_end
        )
        if (item.get("erase_start"), item.get("erase_end")) != (
                erase_start, erase_end):
            raise RuntimeError(f"manifest 的 {file_name} 擦除范围不准确")
        expected_flash[f"0x{offset:x}"] = file_name
    # Check human/tool text paths too, not just JSON offsets.
    with tempfile.TemporaryDirectory() as temp:
        args_path = Path(temp) / "flash_args"
        args_path.write_bytes(payload["flash_args"])
        release._validate_flash_files("ZIP/flash_args", release._parse_text_flash_files(args_path),
                                      {int(key, 0): value for key, value in expected_flash.items()})
    if flat_args.get("flash_files") != expected_flash:
        raise RuntimeError("flasher_args.json 不是六个准确的扁平路径")

    sums = _parse_sums(payload["SHA256SUMS"])
    for name, data in payload.items():
        if name == "SHA256SUMS":
            continue
        if sums.get(name) != _digest(data):
            raise RuntimeError(f"SHA256SUMS 未准确覆盖 {name}")
    extras = set(sums) - (set(payload) - {"SHA256SUMS"})
    if extras:
        raise RuntimeError(f"SHA256SUMS 引用了不存在文件: {', '.join(sorted(extras))}")

    with tempfile.TemporaryDirectory() as tmp:
        table_path = Path(tmp) / "partition-table.bin"
        table_path.write_bytes(payload["partition-table.bin"])
        release._validate_partition_table(table_path)

    if build_dir is not None:
        build_dir = Path(build_dir)
        validated = release.validate_release_inputs(build_dir, "trae_card.bin")
        for segment in validated:
            if payload[segment.file] != segment.source.read_bytes():
                raise RuntimeError(f"ZIP 的 {segment.file} 与 build 输入不一致")
        for name in ("avatar_catalog.json", "petart_catalog.json"):
            if payload[name] != (build_dir / name).read_bytes():
                raise RuntimeError(f"ZIP 的 {name} 与 build 输入不一致")
    if standalone_app is not None:
        standalone_app = Path(standalone_app)
        if not standalone_app.is_file() or standalone_app.read_bytes() != payload["trae_card.bin"]:
            raise RuntimeError("独立 app BIN 与 ZIP/build 应用不一致")
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--standalone-app", type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = verify_archive(args.archive, args.build_dir, args.standalone_app)
    except (RuntimeError, zipfile.BadZipFile) as exc:
        parser.error(str(exc))
    print(
        f"OK: {args.archive} status={manifest['status']} "
        "segments=6 protected-cardid=yes hashes=match"
    )


if __name__ == "__main__":
    main()
