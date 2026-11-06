#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build and package six explicit ESP32-C3 flash segments safely.

The package never creates a raw image spanning per-device or runtime partitions.
`--no-build` packages the current build only and labels it as intermediate.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import zipfile
from dataclasses import dataclass
from pathlib import Path
import sys

if str(Path(__file__).resolve().parents[1]) not in sys.path:
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools import pet_art_pack, companion_pack, body_font_pack

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
RELEASES = ROOT / "releases"
BOARD_CFG = (ROOT / "components" / "platform" / "platform_esp32" /
             "include" / "platform" / "board_config.h")
CHIP = "esp32c3"
SECTOR_SIZE = 0x1000
APP_MIN_FREE_BYTES = 256 * 1024
PROTECTED_RANGES = (("cardid", 0x356000, 0x35A000),)
FONT_LICENSE_FILES = ("LXGW-v1.522-OFL.txt", "LXGW-OFL.txt", "Montserrat-OFL.txt", "FontAwesome5-LICENSE.txt")
FONT_METADATA_FILES = (*FONT_LICENSE_FILES, "FONT-LICENSES.txt", "font-sources.json")
LIBRARY_LICENSE_FILES = ("ESP-AUDIO-CODEC-LICENSE.txt", "PUFF-LICENSE.txt", "SPEEXDSP-LICENSE.txt", "WEBRTC-VAD-LICENSE.txt")


def validate_library_license_payload(payload):
    for name in LIBRARY_LICENSE_FILES:
        expected = (ROOT / "assets/licenses" / name).read_bytes()
        if name not in payload or payload[name] != expected:
            raise RuntimeError(f"库许可缺少或原文不一致: {name}")


def validate_font_license_payload(payload, app):
    """A ZIP and its standalone app must both contain the complete font notices."""
    missing = set(FONT_METADATA_FILES) - set(payload)
    if missing:
        raise RuntimeError(f"字体许可文件缺少: {', '.join(sorted(missing))}")
    combined = payload["FONT-LICENSES.txt"]
    if not combined or combined + b"\0" not in app:
        raise RuntimeError("应用 BIN 缺少完整字体许可原文，不能仅附 ZIP 文本")
    try:
        sources = json.loads(payload["font-sources.json"])
        expected = sources["embedded_notice"]
        if (expected["file"] != "FONT-LICENSES.txt" or expected["bytes"] != len(combined)
                or expected["sha256"] != hashlib.sha256(combined).hexdigest()):
            raise RuntimeError("字体许可合并原文与来源清单不一致")
        notices = {item["file"]: item for item in sources["licenses"]}
        for name in FONT_LICENSE_FILES:
            raw = payload[name]
            item = notices[name]
            if (b"SIL OPEN FONT LICENSE" not in raw or raw not in combined
                    or item["bytes"] != len(raw) or item["sha256"] != hashlib.sha256(raw).hexdigest()):
                raise RuntimeError(f"字体许可原文不完整或哈希不一致: {name}")
    except (ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"字体来源清单无效: {exc}") from exc


def font_license_payload(app):
    payload = {name: (ROOT / "assets/fonts" / name).read_bytes() for name in FONT_METADATA_FILES}
    validate_font_license_payload(payload, app)
    return payload

# This table is a release compatibility contract. A layout change requires an
# explicit release-tool update rather than silently packaging new offsets.
EXPECTED_PARTITIONS = (
    ("nvs", 0x01, 0x02, 0x009000, 0x006000),
    ("phy_init", 0x01, 0x01, 0x00F000, 0x001000),
    ("factory", 0x00, 0x00, 0x010000, 0x300000),
    ("imgstore", 0x01, 0x82, 0x310000, 0x020000),
    ("imgframe", 0x01, 0x82, 0x330000, 0x026000),
    ("cardid", 0x01, 0x02, 0x356000, 0x004000),
    ("imguser", 0x01, 0x82, 0x35A000, 0x020000),
    ("audio", 0x01, 0x82, 0x37A000, 0x080000),
    ("imgava", 0x01, 0x82, 0x3FA000, 0x100000),
    ("petart", 0x01, 0x40, 0x500000, 0x100000),
    ("companion", 0x01, 0x41, 0x600000, 0x200000),
)


@dataclass(frozen=True)
class ReleaseSegment:
    name: str
    file: str
    source: Path
    offset: int
    region_start: int
    region_end: int
    size: int
    erase_start: int
    erase_end: int
    sha256: str


def run(cmd):
    print("· " + " ".join(cmd))
    subprocess.run(cmd, cwd=ROOT, check=True)


def git(*args, default=""):
    try:
        return subprocess.check_output(
            ["git", *args], cwd=ROOT, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return default


def read_flash_size():
    """Read the configured flash size, used only as package metadata."""
    try:
        match = re.search(
            r'CONFIG_ESPTOOLPY_FLASHSIZE="([^"]+)"',
            (ROOT / "sdkconfig").read_text(encoding="utf-8"),
        )
        return match.group(1) if match else "unknown"
    except OSError:
        return "unknown"


def read_periph_tag():
    """Read enabled PERIPH_* switches for the artifact name."""
    short = {"DISPLAY": "disp", "AUDIO": "aud", "BUTTON": "btn",
             "BLE": "ble", "BATTERY": "bat", "LED": "led"}
    enabled = []
    with BOARD_CFG.open(encoding="utf-8") as stream:
        for line in stream:
            match = re.match(r"\s*#define\s+PERIPH_(\w+)\s+(\d)", line)
            if match and match.group(2) == "1" and match.group(1) in short:
                enabled.append(short[match.group(1)])
    order = ["disp", "aud", "btn", "ble", "bat", "led"]
    enabled.sort(key=lambda item: order.index(item))
    return "-".join(enabled) if enabled else "none"


def _overlaps(start_a, end_a, start_b, end_b):
    return start_a < end_b and end_a > start_b


def validate_write_extent(name, offset, size, region_start, region_end):
    """Validate raw bytes and their 4 KiB erase-sector expansion."""
    if size <= 0:
        raise RuntimeError(f"{name} 为空")
    end = offset + size
    if offset < region_start or end > region_end:
        raise RuntimeError(
            f"{name} 超出 {name.split()[0]} 分区容量: "
            f"0x{offset:x}..0x{end - 1:x}, allowed 0x{region_start:x}..0x{region_end - 1:x}"
        )
    erase_start = offset & ~(SECTOR_SIZE - 1)
    erase_end = (end + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1)
    if erase_start < region_start or erase_end > region_end:
        raise RuntimeError(
            f"{name} 的 4KiB 擦除范围 0x{erase_start:x}..0x{erase_end - 1:x} "
            "超出允许分区"
        )
    for protected_name, protected_start, protected_end in PROTECTED_RANGES:
        if _overlaps(erase_start, erase_end, protected_start, protected_end):
            raise RuntimeError(
                f"{name} 的擦除范围触及保护区 {protected_name} "
                f"0x{protected_start:x}..0x{protected_end - 1:x}"
            )
    return erase_start, erase_end


def _parse_partition_table(path):
    data = path.read_bytes()
    if len(data) != 0xC00:
        raise RuntimeError(
            f"partition-table.bin 长度错误: 0x{len(data):x}, expected 0xc00"
        )
    entries = []
    for pos in range(0, len(data), 32):
        magic = struct.unpack_from("<H", data, pos)[0]
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            raise RuntimeError(f"partition-table.bin entry 0x{pos:x} magic 错误")
        _, kind, subtype, offset, size, raw_label, _flags = struct.unpack_from(
            "<HBBII16sI", data, pos
        )
        label = raw_label.split(b"\0", 1)[0].decode("ascii", errors="strict")
        expected_flags = 2 if label in ("petart", "companion") else 0
        if _flags != expected_flags:
            raise RuntimeError(f"partition-table.bin {label} flags 错误: {_flags}, expected {expected_flags}")
        entries.append((label, kind, subtype, offset, size))
    return tuple(entries)


def _validate_partition_table(path):
    actual = _parse_partition_table(path)
    if len(actual) != len(EXPECTED_PARTITIONS):
        raise RuntimeError(
            f"固定 partition map 数量错误: {len(actual)}, expected {len(EXPECTED_PARTITIONS)}"
        )
    for expected, found in zip(EXPECTED_PARTITIONS, actual):
        if expected != found:
            name = expected[0]
            raise RuntimeError(
                f"固定 partition map 的 {name} 不匹配: got {found}, expected {expected}"
            )


def _parse_text_flash_files(path):
    tokens = path.read_text(encoding="utf-8").split()
    result = {}
    for index, token in enumerate(tokens[:-1]):
        if not re.fullmatch(r"0x[0-9a-fA-F]+", token):
            continue
        offset = int(token, 0)
        if offset in result:
            raise RuntimeError(f"build/flash_args 重复 offset 0x{offset:x}")
        result[offset] = Path(tokens[index + 1]).name
    return result


def _parse_json_flash_files(path):
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))["flash_files"]
        result = {int(offset, 0): Path(name).name for offset, name in raw.items()}
    except (OSError, ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"build/flasher_args.json 无效: {exc}") from exc
    return result


def _expected_flash_files(app_bin):
    return {
        0x000000: "bootloader.bin",
        0x008000: "partition-table.bin",
        0x010000: Path(app_bin).name,
        0x3FA000: "imgava.bin",
        0x500000: "petart.bin",
        0x600000: "companion.bin",
    }


def _validate_flash_files(source_name, actual, expected):
    if actual == expected:
        return
    actual_desc = ", ".join(
        f"0x{offset:x}:{name}" for offset, name in sorted(actual.items())
    )
    expected_desc = ", ".join(
        f"0x{offset:x}:{name}" for offset, name in sorted(expected.items())
    )
    raise RuntimeError(
        f"{source_name} 的文件或 offset/偏移错误: got [{actual_desc}], "
        f"expected [{expected_desc}]"
    )


def _segment(name, archive_name, source, offset, region_start, region_end):
    if not source.is_file():
        raise RuntimeError(f"发布包缺少 {archive_name}: {source}")
    size = source.stat().st_size
    erase_start, erase_end = validate_write_extent(
        f"{name} {archive_name}", offset, size, region_start, region_end
    )
    digest = hashlib.sha256(source.read_bytes()).hexdigest()
    return ReleaseSegment(
        name=name,
        file=archive_name,
        source=source,
        offset=offset,
        region_start=region_start,
        region_end=region_end,
        size=size,
        erase_start=erase_start,
        erase_end=erase_end,
        sha256=digest,
    )


def validate_release_inputs(build_dir, app_bin):
    """Validate the locked map, six build segments, sizes and write offsets."""
    build_dir = Path(build_dir)
    app_name = Path(app_bin).name
    expected_flash = _expected_flash_files(app_name)
    required = (
        build_dir / "avatar_catalog.json",
        build_dir / "petart_catalog.json",
        build_dir / "flash_args",
        build_dir / "flasher_args.json",
    )
    for path in required:
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"发布包缺少有效的 {path.name}")

    partition_path = build_dir / "partition_table" / "partition-table.bin"
    if not partition_path.is_file():
        raise RuntimeError("发布包缺少 partition-table.bin")
    _validate_partition_table(partition_path)
    _validate_flash_files(
        "build/flash_args", _parse_text_flash_files(build_dir / "flash_args"),
        expected_flash,
    )
    _validate_flash_files(
        "build/flasher_args.json",
        _parse_json_flash_files(build_dir / "flasher_args.json"), expected_flash,
    )

    segments = (
        _segment("bootloader", "bootloader.bin",
                 build_dir / "bootloader" / "bootloader.bin",
                 0x000000, 0x000000, 0x008000),
        _segment("partition-table", "partition-table.bin", partition_path,
                 0x008000, 0x008000, 0x009000),
        _segment("factory", app_name, build_dir / app_name,
                 0x010000, 0x010000, 0x310000),
        _segment("imgava", "imgava.bin", build_dir / "imgava.bin",
                 0x3FA000, 0x3FA000, 0x4FA000),
        _segment("petart", "petart.bin", build_dir / "petart.bin",
                 0x500000, 0x500000, 0x600000),
        _segment("companion", "companion.bin", build_dir / "companion" / "companion.bin",
                 0x600000, 0x600000, 0x800000),
    )
    app_free = segments[2].region_end - segments[2].region_start - segments[2].size
    if app_free < APP_MIN_FREE_BYTES:
        raise RuntimeError(
            f"应用分区余量不足: {app_free} B，发布至少需要 {APP_MIN_FREE_BYTES} B；"
            "请使用 SIZE 编译优化并检查资源增长，不要移动 cardid 分区"
        )
    if segments[1].size != 0xC00:
        raise RuntimeError("partition-table.bin 必须是 0xc00 字节")
    if segments[3].size != 0x100000:
        raise RuntimeError("imgava.bin 必须完整覆盖 0x100000 字节 imgava 分区")
    if segments[0].source.read_bytes()[:1] != b"\xe9":
        raise RuntimeError("bootloader.bin 不是有效 ESP 镜像")
    if segments[2].source.read_bytes()[:1] != b"\xe9":
        raise RuntimeError(f"{app_name} 不是有效 ESP 应用镜像")
    if segments[3].source.read_bytes()[:4] != b"AVA1":
        raise RuntimeError("imgava.bin 缺少 AVA1 文件头")
    shared_catalog = validate_pet_art_payload(
        segments[4].source.read_bytes(),
        (build_dir / "petart_catalog.json").read_bytes(), segments[2].source.read_bytes())
    try:
        pet_art_pack.validate_shared_game_catalog(shared_catalog)
    except ValueError as exc:
        raise RuntimeError(f"shared game petart.bin invalid: {exc}") from exc
    validate_companion_payload(segments[5].source.read_bytes(), segments[2].source.read_bytes())
    return segments


def validate_companion_payload(data, app):
    try:
        companion_pack.validate(data)
    except ValueError as exc:
        raise RuntimeError(f"companion.bin invalid: {exc}") from exc
    used = struct.unpack_from("<I", data, 8)[0]
    fingerprint = ("COMPANION-SHA256:" + hashlib.sha256(data[:used]).hexdigest()).encode()
    if fingerprint + b"\0" not in app:
        raise RuntimeError("app / companion.bin fingerprint mismatch")
    if struct.unpack_from('<I', data, 4)[0] == 3:
        at, length = struct.unpack_from('<II', data, 16)
        body_id = ('BODYFONT-SHA256:' + data[at:at+length].hex()).encode() + b'\0'
        if body_id not in app:
            raise RuntimeError('app / companion body font fingerprint mismatch')


def validate_pet_art_payload(data, catalog_bytes, app):
    try:
        catalog = json.loads(catalog_bytes)
        sprites, font = body_font_pack.split(data, catalog)
        pet_art_pack.validate_pack(sprites, catalog)
    except (ValueError, KeyError, TypeError) as exc:
        raise RuntimeError(f"petart.bin / catalog 无效: {exc}") from exc
    if pet_art_pack.fingerprint(sprites).encode("ascii") + b"\0" not in app:
        raise RuntimeError("应用 BIN 与 petart.bin 不匹配：缺少匹配的 PETART-SHA256 指纹")
    if font is not None:
        if body_font_pack.fingerprint(font).encode() + b'\0' not in app:
            raise RuntimeError('app / body font fingerprint mismatch')
    elif b'BODYFONT-SHA256:' in app:
        raise RuntimeError('application requires missing body font')
    return catalog


# Kept as a compatibility entry point for older callers.
def validate_avatar_release_inputs(build_dir):
    validate_release_inputs(build_dir, "trae_card.bin")


def _json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()


def _flat_flash_args(segments):
    lines = ["--flash_mode dio --flash_freq 80m --flash_size detect"]
    lines.extend(f"0x{segment.offset:x} {segment.file}" for segment in segments)
    return ("\n".join(lines) + "\n").encode()


def _flasher_args(segments, target):
    files = {f"0x{segment.offset:x}": segment.file for segment in segments}
    return {
        "write_flash_args": [
            "--flash_mode", "dio", "--flash_size", "detect",
            "--flash_freq", "80m",
        ],
        "flash_settings": {
            "flash_mode": "dio", "flash_size": "detect", "flash_freq": "80m",
        },
        "flash_files": files,
        "extra_esptool_args": {
            "after": "hard_reset", "before": "default_reset",
            "stub": True, "chip": target,
        },
    }


def _firmware_manifest(segments, metadata):
    return {
        "schema_version": 1,
        "features": {"pet": False},
        "package_type": "explicit-flash-segments",
        "status": metadata["status"],
        "version": metadata["version"],
        "target": metadata["target"],
        "flash_size": metadata["flash_size"],
        "periph": metadata["periph"],
        "source": {
            "branch": metadata["branch"],
            "channel": metadata["channel"],
            "git": metadata["git"],
        },
        "protected_ranges": [
            {"name": name, "start": start, "end": end}
            for name, start, end in PROTECTED_RANGES
        ],
        "preserved_partitions": [
            "nvs", "phy_init", "imgstore", "imgframe", "cardid",
            "imguser", "audio",
        ],
        "standalone_application": {
            "file": segments[2].file,
            "offset": segments[2].offset,
            "sha256": segments[2].sha256,
            "requires_matching_shared_game_art": {
                "file": segments[4].file, "offset": segments[4].offset,
                "sha256": segments[4].sha256,
            },
            "requires_matching_companion": {"file": segments[5].file, "offset": segments[5].offset, "sha256": segments[5].sha256},
            "complete_flash_image": False,
        },
        "segments": [
            {
                "name": segment.name,
                "file": segment.file,
                "offset": segment.offset,
                "size": segment.size,
                "end": segment.offset + segment.size,
                "erase_start": segment.erase_start,
                "erase_end": segment.erase_end,
                "sha256": segment.sha256,
            }
            for segment in segments
        ],
    }


def _human_manifest(segments, metadata):
    status_note = (
        "INTERMEDIATE SNAPSHOT：由 --no-build 使用现有 build 生成，不是最终发布包。"
        if metadata["status"] == "intermediate"
        else "Release package：由本次 release.py 构建结果生成。"
    )
    segment_args = " \\\n  ".join(
        f"0x{segment.offset:x} {segment.file}" for segment in segments
    )
    rows = "\n".join(
        f"- {segment.file}: offset=0x{segment.offset:x}, size=0x{segment.size:x}, "
        f"erase=0x{segment.erase_start:x}..0x{segment.erase_end - 1:x}, "
        f"sha256={segment.sha256}"
        for segment in segments
    )
    return f"""AI PASSPORT 显式分段固件包
================================
{status_note}
版本: {metadata['version']}
芯片 / Flash: {metadata['target']} / {metadata['flash_size']}
外设: {metadata['periph']}
分支 / 渠道: {metadata['branch']} / {metadata['channel']}
Git: {metadata['git']}

六段写入命令（文件均位于 ZIP 根目录）:
  esptool.py --chip {metadata['target']} write_flash --flash_mode dio --flash_freq 80m --flash_size detect \\
  {segment_args}

写入段:
{rows}

独立应用 BIN: {segments[2].file}，仅写入 offset 0x10000，必须配合本包 petart.bin（0x500000）和 companion.bin（0x600000）使用。
此版本已移除宠物功能；petart.bin 保留主页默认形象与躲避游戏的 7 张共享素材，并包含配套的正文中文字库。
应用会拒绝缺失、损坏或不匹配的共享图包；只有应用 BIN 不构成完整刷写包。
petart 沿用已部署的分区名称，是只读共享资源分区，禁止改成跨越 cardid 的单个 raw BIN；网页刷写器必须支持这六个显式段。
禁止整片擦除。cardid 保护区 0x356000..0x359fff，以及 NVS、用户图片、音频等运行时分区均不在六个写入段或其 4KiB 擦除范围内。
firmware-manifest.json 记录每段 offset/size/erase extent/SHA-256；SHA256SUMS 可校验 ZIP 内文件。
FONT-LICENSES.txt 保留完整字体版权、许可与来源，独立应用 BIN 内也包含同一原文。
PUFF-LICENSE.txt 保留字形块 DEFLATE 解码器的完整许可、固定来源提交与修改说明。
SPEEXDSP-LICENSE.txt 与 WEBRTC-VAD-LICENSE.txt 保留回声消除、语音检测代码的完整许可与来源。
设备主页长按确定可离线分页查看；字体 OFL 仅适用于字体数据，不改变整个固件的许可。
"""


def write_release_artifacts(build_dir, archive_path, standalone_app_path,
                            app_bin, metadata):
    """Validate build inputs, then atomically write a flat ZIP and app BIN."""
    build_dir = Path(build_dir)
    archive_path = Path(archive_path)
    standalone_app_path = Path(standalone_app_path)
    segments = validate_release_inputs(build_dir, app_bin)
    archive_path.parent.mkdir(parents=True, exist_ok=True)
    standalone_app_path.parent.mkdir(parents=True, exist_ok=True)

    manifest = _firmware_manifest(segments, metadata)
    payload = {
        segment.file: segment.source.read_bytes() for segment in segments
    }
    payload.update(font_license_payload(payload[segments[2].file]))
    payload.update({name: (ROOT / "assets/licenses" / name).read_bytes() for name in LIBRARY_LICENSE_FILES})
    validate_library_license_payload(payload)
    for name, notice in (("farewell", "FAREWELL-SOURCE.txt"), ("welcome", "WELCOME-SOURCE.txt"), ("startup", "BOOT-SOURCE.txt")):
        suffix, audio_format, rate = ("pcm", "PCM-S16LE", 24000) if name == "welcome" else ("adpcm", "IMA-ADPCM", 16000)
        prompt_file = "startup_magicbox.adpcm" if name == "startup" else f"{name}_zh.{suffix}"
        prompt_path = ROOT / "components/platform/xiaozhi" / prompt_file
        if not prompt_path.is_file():
            continue
        prompt = prompt_path.read_bytes()
        offset = payload[segments[2].file].find(prompt) if prompt else -1
        if name == "startup" and metadata.get("version", "").startswith("2.1.0-xiaozhi.") and int(metadata["version"].rsplit(".", 1)[1]) >= 11:
            if offset < 0 or len(prompt) != 8960:
                raise RuntimeError("开机音乐未正确嵌入应用，拒绝发布")
        if offset >= 0:
            payload[notice] = (ROOT / "assets/licenses" / notice).read_bytes()
            manifest[f"{name}_audio"] = {"format": audio_format, "sample_rate": rate,
                                        "bytes": len(prompt), "app_offset": offset, "sha256": hashlib.sha256(prompt).hexdigest(),
                                        "notice": notice}
    manifest["font_notices"] = {
        "file": "FONT-LICENSES.txt",
        "sha256": hashlib.sha256(payload["FONT-LICENSES.txt"]).hexdigest(),
        "embedded_in_application": True,
        "device_reader": "Home > hold OK",
        "scope": "font software and derived glyph data only",
    }
    payload["avatar_catalog.json"] = (build_dir / "avatar_catalog.json").read_bytes()
    payload["petart_catalog.json"] = (build_dir / "petart_catalog.json").read_bytes()
    pet_catalog = validate_pet_art_payload(payload["petart.bin"], payload["petart_catalog.json"], payload[segments[2].file])
    manifest["shared_game_art"] = {
        "uses": ["dodge", "home-avatar"],
        "file": "petart.bin", "catalog": "petart_catalog.json", "format": "PTP1",
        "offset": 0x500000, "capacity": pet_art_pack.CAPACITY,
        "bytes": len(payload["petart.bin"]), "count": pet_catalog["count"],
        "sha256": hashlib.sha256(payload["petart.bin"]).hexdigest(), "mapping": "read-only Flash, native PSR1 row decode",
    }
    if 'body_font' in pet_catalog:
        manifest['body_font'] = dict(pet_catalog['body_font'], file='petart.bin', offset=0x510000)
    payload["flash_args"] = _flat_flash_args(segments)
    payload["flasher_args.json"] = _json_bytes(
        _flasher_args(segments, metadata["target"])
    )
    payload["firmware-manifest.json"] = _json_bytes(manifest)
    payload["MANIFEST.txt"] = _human_manifest(segments, metadata).encode("utf-8")
    sums = "".join(
        f"{hashlib.sha256(data).hexdigest()}  {name}\n"
        for name, data in sorted(payload.items())
    )
    payload["SHA256SUMS"] = sums.encode("ascii")

    temp_zip = archive_path.with_name(archive_path.name + ".tmp")
    temp_app = standalone_app_path.with_name(standalone_app_path.name + ".tmp")
    try:
        with zipfile.ZipFile(temp_zip, "w", zipfile.ZIP_DEFLATED) as package:
            for name, data in payload.items():
                if Path(name).name != name:
                    raise RuntimeError(f"ZIP 路径必须扁平: {name}")
                package.writestr(name, data)
        shutil.copyfile(segments[2].source, temp_app)
        os.replace(temp_zip, archive_path)
        os.replace(temp_app, standalone_app_path)
    finally:
        temp_zip.unlink(missing_ok=True)
        temp_app.unlink(missing_ok=True)
    return segments


def _artifact_paths(base):
    return RELEASES / f"{base}.zip", RELEASES / f"{base}-app-0x10000.bin"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--no-build", action="store_true",
        help="package the existing build as an intermediate snapshot",
    )
    args = parser.parse_args(argv)

    if not args.no_build:
        run(["idf.py", "build"])
    if not BUILD.is_dir():
        parser.error("没有 build/；请先构建或移除 --no-build")

    try:
        desc = json.loads((BUILD / "project_description.json").read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        parser.error(f"build/project_description.json 无效: {exc}")
    version = desc.get("project_version") or "0.0.0"
    app_bin = Path(desc.get("app_bin", "trae_card.bin")).name
    target = desc.get("target", CHIP)
    flash_size = read_flash_size()
    periph = read_periph_tag()

    branch = git("rev-parse", "--abbrev-ref", "HEAD", default="nogit")
    short_hash = git("rev-parse", "--short", "HEAD", default="0000000")
    dirty = "+dirty" if git("status", "--porcelain") else ""
    channel = "release" if branch == "main" else "beta"
    status = "intermediate" if args.no_build else "release"
    status_tag = "_intermediate" if args.no_build else ""

    RELEASES.mkdir(parents=True, exist_ok=True)
    stem = (
        f"ai_passport_{target}_{flash_size}_{periph}_{channel}{status_tag}_"
        f"{short_hash}{dirty}_v{version}"
    )
    if channel == "beta":
        number = 1
        while True:
            base = f"{stem}-beta.{number}"
            archive_path, app_path = _artifact_paths(base)
            if not archive_path.exists() and not app_path.exists():
                break
            number += 1
    else:
        base = stem
        archive_path, app_path = _artifact_paths(base)
        if archive_path.exists() or app_path.exists():
            parser.error(f"同名发布产物已存在: {archive_path} / {app_path}")

    metadata = {
        "version": version,
        "target": target,
        "flash_size": flash_size,
        "periph": periph,
        "branch": branch,
        "channel": channel,
        "git": short_hash + dirty,
        "status": status,
    }
    try:
        segments = write_release_artifacts(
            BUILD, archive_path, app_path, app_bin=app_bin, metadata=metadata
        )
    except RuntimeError as exc:
        parser.error(str(exc))

    print("\n✓ 六段安全包已生成:")
    print(f"  ZIP: {archive_path} ({archive_path.stat().st_size // 1024} KB)")
    print(f"  App: {app_path} (offset 0x10000, sha256 {segments[2].sha256})")
    print("  写入 offsets: " + ", ".join(hex(s.offset) for s in segments))
    if args.no_build:
        print("  ⚠ INTERMEDIATE SNAPSHOT：使用现有 build；最终发布需由最终源码重新构建打包")
    elif dirty:
        print("  ⚠ 工作区有未提交改动(+dirty)，正式发布建议使用审定后的干净源码")
    return archive_path, app_path


if __name__ == "__main__":
    main()
