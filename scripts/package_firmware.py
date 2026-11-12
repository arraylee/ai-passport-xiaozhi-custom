#!/usr/bin/env python3
"""Package the same version as a merged Recovery image and six USB segments."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
from scripts import release
from tools import pack_miniapp, verify_release


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_zip(path, payload):
    payload = dict(payload)
    payload['SHA256SUMS'] = ''.join(
        f'{digest(data)}  {name}\n' for name, data in sorted(payload.items())
        if name != 'SHA256SUMS').encode()
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(payload.items()):
            if Path(name).name != name:
                raise ValueError('Package members must be flat files')
            archive.writestr(name, data)
    with zipfile.ZipFile(path) as archive:
        assert archive.testzip() is None
        assert all(archive.read(name) == data for name, data in payload.items())


def build(usb, mini, out, minimum, report=None):
    usb, mini, out = map(Path, (usb, mini, out))
    descriptions = [json.loads((d / 'project_description.json').read_text()) for d in (usb, mini)]
    version = descriptions[0]['project_version']
    if descriptions[1]['project_version'] != version or any(d['target'] != 'esp32c3' for d in descriptions):
        raise ValueError('USB and mini-program builds must have the same version and chip')
    for directory in (usb, mini):
        app = (directory / 'trae_card.bin').read_bytes()
        pack_miniapp.verify_esp(app)
        pack_miniapp.verify_esp((directory / 'bootloader/bootloader.bin').read_bytes())
        if (version + '\0').encode() not in app[:256]:
            raise ValueError('Build description does not match actual firmware version')
    image = bytes(pack_miniapp.build(mini))
    out.mkdir(parents=True, exist_ok=True)
    stem = 'AI-PASSPORT-Xiaozhi-' + version
    segment_zip = out / (stem + '-partitions.zip')
    app_path = out / (stem + '-app-0x10000.bin')
    metadata = dict(version=version, target='esp32c3', flash_size='8MB',
                    periph=release.read_periph_tag(), branch=release.git('branch', '--show-current'),
                    channel='release', git=release.git('rev-parse', 'HEAD') +
                    ('+dirty' if release.git('status', '--porcelain') else ''), status='verified-build')
    segments = release.write_release_artifacts(usb, segment_zip, app_path, 'trae_card.bin', metadata,
                                               min_app_free_bytes=minimum)
    with zipfile.ZipFile(segment_zip) as archive:
        payload = {name: archive.read(name) for name in archive.namelist()}
    rows = '\n'.join(f'{s.file:22}  0x{s.offset:06X}  {s.size} bytes' for s in segments)
    payload['FLASH-ADDRESSES.csv'] = ('file,offset_hex,size_bytes,sha256\n' + ''.join(
        f'{s.file},0x{s.offset:06X},{s.size},{s.sha256}\n' for s in segments)).encode()
    payload['INSTALL.txt'] = f'''AI PASSPORT 小智版 {version} — 生产 / USB 分区烧录包

适用：ESP32-C3，8 MB Flash。六段 BIN 均为本版原始构建文件，未二次修改。
解压后文件均在同一目录。ESP Flash Download Tool 选择 ESP32-C3、DIO、80 MHz、8 MB，
按下表勾选六个文件和地址；也可在本目录运行 esptool（将 PORT 换成实际串口）：

python -m esptool --chip esp32c3 --port PORT --baud 460800 write_flash @flash_args

{rows}

空白新板可直接烧录这六段。完成后重启，出现 FoloToy 开机画面并进入小智首次配置。
已使用相同传统 USB 分区的板卡，可保留原 NVS、设备身份及用户数据；不要整片擦除。
其他玩法或小程序安装器的分区布局不同，不能按保留配置升级处理；先依产线流程核对并初始化旧布局。
本包不含每卡独立的身份、密钥、Wi-Fi、绑定信息或用户主题；设备身份仍按产线流程单独写入。
不要只烧录 trae_card.bin 而遗漏配套字库和形象资源，也不要将 miniapp.bin 混入此地址表。

SHA256SUMS、firmware-manifest.json 和 FLASH-ADDRESSES.csv 用于归档与校验。
应用分区实际余量 {segments[2].region_end - segments[2].region_start - segments[2].size} 字节，
本次显式声明的 Flash 增长预留为 {minimum} 字节；这是存储预算，不是运行内存或 OTA 备用区。
'''.encode()
    if report:
        payload['TEST-REPORT.txt'] = Path(report).read_bytes()
    write_zip(segment_zip, payload)
    verify_release.verify_archive(segment_zip, usb, app_path)

    image_name = stem + '-miniapp.bin'
    (out / image_name).write_bytes(image)
    merged_zip = out / (stem + '-merged.zip')
    notes = {name: value for name, value in payload.items()
             if name in set(release.FONT_METADATA_FILES) | set(release.LIBRARY_LICENSE_FILES) |
             set(release.THEME_NOTICES) | {'BOOT-SOURCE.txt', 'WELCOME-SOURCE.txt', 'FAREWELL-SOURCE.txt', 'TEST-REPORT.txt'}}
    notes[image_name] = image
    notes['INSTALL.txt'] = f'''AI PASSPORT 小智版 {version} — 小程序合并固件包

{image_name}：{len(image)} 字节（{len(image) / 1000000:.2f} MB）。
适用于已安装协议 4 常驻 Recovery 的 AI Passport，由微信小程序玩法安装器下载。
这是完整玩法镜像，含分区表、应用、字库与形象资源；不含常驻 Recovery 安装器本体。
它不是传统 USB 版的整片镜像，不要写到地址 0 或当作应用 BIN 写入 0x10000。
生产直接通过 USB 烧录请使用同版 -partitions.zip 中的六段文件与地址表。
小程序换装会重置本地配置；首次使用按卡上指引完成配网和绑定。
两个包的玩法版本相同，但资源排列不同，不能互相混用文件。
本地打包或 GitHub 发布不代表社区玩法 72 已更新。
'''.encode()
    notes['firmware-manifest.json'] = release._json_bytes(dict(
        version=version, package_type='miniapp-recovery-merged', target='esp32c3',
        bytes=len(image), sha256=digest(image), file=image_name, requires='Protocol 4 resident Recovery',
        source=metadata['git']))
    write_zip(merged_zip, notes)
    summary = [dict(file=p.name, bytes=p.stat().st_size, sha256=digest(p.read_bytes()))
               for p in (merged_zip, segment_zip, out / image_name)]
    (out / 'SHA256SUMS.txt').write_text(''.join(f"{s['sha256']}  {s['file']}\n" for s in summary))
    (out / 'packages.json').write_bytes(release._json_bytes(dict(version=version, files=summary)))
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--usb-build', type=Path, default=ROOT / 'build')
    parser.add_argument('--miniapp-build', type=Path, default=ROOT / 'build-miniapp')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--min-usb-free-bytes', type=lambda s: int(s, 0), default=release.APP_MIN_FREE_BYTES)
    parser.add_argument('--test-report', type=Path)
    args = parser.parse_args()
    build(args.usb_build, args.miniapp_build, args.output, args.min_usb_free_bytes, args.test_report)
