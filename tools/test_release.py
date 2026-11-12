import hashlib
import json
import struct
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts import release
from tools import verify_release, pet_art_pack, companion_pack


EXPECTED_PARTITIONS = (
    ("nvs", 0x01, 0x02, 0x009000, 0x006000),
    ("phy_init", 0x01, 0x01, 0x00F000, 0x001000),
    ("factory", 0x00, 0x00, 0x010000, 0x300000),
    ("imgstore", 0x01, 0x82, 0x310000, 0x020000),
    ("imgframe", 0x01, 0x82, 0x330000, 0x026000),
    ("cardid", 0x01, 0x02, 0x356000, 0x004000),
    ("imguser", 0x01, 0x82, 0x35A000, 0x020000),
    ("audio", 0x01, 0x82, 0x37A000, 0x080000),
    ("imgava", 0x01, 0x82, 0x3FA000, 0x020000),
    ("assets", 0x01, 0x82, 0x41A000, 0x0E6000),
    ("petart", 0x01, 0x40, 0x500000, 0x0E9000),
    ("theme_cache", 0x01, 0x42, 0x5E9000, 0x017000),
    ("companion", 0x01, 0x41, 0x600000, 0x200000),
)


def partition_table(entries=EXPECTED_PARTITIONS):
    data = bytearray(b"\xff" * 0xC00)
    for index, (name, kind, subtype, offset, size) in enumerate(entries):
        label = name.encode("ascii").ljust(16, b"\0")
        struct.pack_into("<HBBII16sI", data, index * 32, 0x50AA, kind, subtype,
                         offset, size, label, 2 if name in ("petart", "companion") else 0)
    return bytes(data)


def write_fixture(root):
    build = Path(root)
    (build / "bootloader").mkdir(parents=True)
    (build / "partition_table").mkdir()
    (build / "bootloader" / "bootloader.bin").write_bytes(b"\xe9" + b"B" * 0x521F)
    (build / "partition_table" / "partition-table.bin").write_bytes(partition_table())
    notice = (release.ROOT / "assets/fonts/FONT-LICENSES.txt").read_bytes()
    sprite = b"PSR1\1\0\0\0" + b"\0\0\0\xff" + b"\0\0\2\0" + b"\x80\0"
    pet_data, pet_catalog = pet_art_pack.build_pack([(name, 1, 1, sprite) for name in pet_art_pack.SHARED_GAME_SYMBOLS])
    pet_catalog['capacity'] = 0xe9000
    (build / "petart.bin").write_bytes(pet_data)
    (build / "petart_catalog.json").write_text(json.dumps(pet_catalog))
    companion = bytearray(128)
    companion[:4] = b"CPR1"
    for i in range(14):
        n = 4 if i < 2 else 144 * 144 * 3
        struct.pack_into("<II", companion, 16+i*8, len(companion), n)
        companion.extend(bytes(n))
    import zlib
    struct.pack_into("<III", companion, 4, 2, len(companion), zlib.crc32(companion[16:]))
    companion_fingerprint = ("COMPANION-SHA256:" + hashlib.sha256(companion).hexdigest()).encode() + b"\0"
    companion.extend(b"\xff" * (0x200000-len(companion)))
    (build / "companion").mkdir()
    (build / "companion" / "companion.bin").write_bytes(companion)
    fingerprint = pet_art_pack.fingerprint(pet_data).encode() + b"\0" + companion_fingerprint
    (build / "trae_card.bin").write_bytes(b"\xe9" + b"A" * 0x1FFFF + notice + b"\0" + fingerprint)
    (build / "imgava.bin").write_bytes(b"AVA1" + b"\xff" * (0x20000 - 4))
    (build / "avatar_catalog.json").write_text('{"avatars": []}\n', encoding="utf-8")
    (build / "project_description.json").write_text(json.dumps({
        "project_version": "1.2.0",
        "app_bin": "trae_card.bin",
        "target": "esp32c3",
    }), encoding="utf-8")
    (build / "flash_args").write_text(
        "--flash_mode dio --flash_freq 80m --flash_size detect\n"
        "0x0 bootloader/bootloader.bin\n"
        "0x8000 partition_table/partition-table.bin\n"
        "0x10000 trae_card.bin\n"
        "0x3fa000 imgava.bin\n"
        "0x500000 petart.bin\n"
        "0x600000 companion/companion.bin\n",
        encoding="utf-8",
    )
    (build / "flasher_args.json").write_text(json.dumps({
        "flash_files": {
            "0x0": "bootloader/bootloader.bin",
            "0x8000": "partition_table/partition-table.bin",
            "0x10000": "trae_card.bin",
            "0x3fa000": "imgava.bin",
            "0x500000": "petart.bin",
            "0x600000": "companion/companion.bin",
        }
    }), encoding="utf-8")
    return build


class ReleaseValidationTests(unittest.TestCase):
    def test_app_requires_256k_release_headroom(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            app = build / "trae_card.bin"
            original = app.read_bytes()
            limit = 0x300000 - release.APP_MIN_FREE_BYTES
            app.write_bytes(original.ljust(limit, b"\0"))
            release.validate_release_inputs(build, "trae_card.bin")
            app.write_bytes(original.ljust(limit + 1, b"\0"))
            with self.assertRaisesRegex(RuntimeError, "余量不足"):
                release.validate_release_inputs(build, "trae_card.bin")

    def test_font_delivery_rejects_bin_without_complete_notices(self):
        with self.assertRaisesRegex(RuntimeError, "字体|许可"):
            release.font_license_payload(b"\xe9incomplete firmware")

    def test_explicit_growth_reserve_keeps_hard_partition_bounds(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            app = build / "trae_card.bin"
            original = app.read_bytes()
            limit = 0x300000 - 192 * 1024
            app.write_bytes(original.ljust(limit, b"\0"))
            with self.assertRaisesRegex(RuntimeError, "余量不足"):
                release.validate_release_inputs(build, "trae_card.bin")
            release.validate_release_inputs(build, "trae_card.bin", 192 * 1024)
            app.write_bytes(original.ljust(limit + 1, b"\0"))
            with self.assertRaisesRegex(RuntimeError, "余量不足"):
                release.validate_release_inputs(build, "trae_card.bin", 192 * 1024)
            app.write_bytes(original.ljust(0x300001, b"\0"))
            with self.assertRaisesRegex(RuntimeError, "超出"):
                release.validate_release_inputs(build, "trae_card.bin", 0)
            for reserve in (-1, 0x300001):
                with self.assertRaisesRegex(RuntimeError, "余量无效"):
                    release.validate_release_inputs(build, "trae_card.bin", reserve)

    def test_font_delivery_keeps_original_notices_and_bin_bytes(self):
        expected = (release.ROOT / "assets/fonts/FONT-LICENSES.txt").read_bytes()
        payload = release.font_license_payload(b"\xe9app" + expected + b"\x00")
        self.assertEqual(expected, payload["FONT-LICENSES.txt"])
        for name in release.FONT_LICENSE_FILES:
            original = (release.ROOT / "assets/fonts" / name).read_bytes()
            self.assertEqual(original, payload[name])
            self.assertIn(original, expected)

    def test_puff_source_notice_is_required_and_exact(self):
        payload = {name: (release.ROOT / "assets/licenses" / name).read_bytes()
                   for name in release.LIBRARY_LICENSE_FILES}
        release.validate_library_license_payload(payload)
        for damage in ("missing", "altered"):
            changed = dict(payload)
            if damage == "missing": changed.pop("PUFF-LICENSE.txt")
            else: changed["PUFF-LICENSE.txt"] = b"truncated attribution"
            with self.subTest(damage=damage), self.assertRaisesRegex(RuntimeError, "PUFF-LICENSE"):
                release.validate_library_license_payload(changed)

    def test_rejects_missing_segment(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            (build / "trae_card.bin").unlink()
            with self.assertRaisesRegex(RuntimeError, "trae_card.bin"):
                release.validate_release_inputs(build, "trae_card.bin")

    def test_rejects_oversized_segment(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            (build / "trae_card.bin").write_bytes(b"\xe9" + b"X" * 0x300000)
            with self.assertRaisesRegex(RuntimeError, "factory.*容量|容量.*factory"):
                release.validate_release_inputs(build, "trae_card.bin")

    def test_rejects_wrong_flash_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            data = json.loads((build / "flasher_args.json").read_text(encoding="utf-8"))
            data["flash_files"]["0x11000"] = data["flash_files"].pop("0x10000")
            (build / "flasher_args.json").write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "offset|偏移"):
                release.validate_release_inputs(build, "trae_card.bin")

    def test_rejects_protected_overlap_after_sector_expansion(self):
        with self.assertRaisesRegex(RuntimeError, "cardid|保护"):
            release.validate_write_extent("bad.bin", 0x356001, 1, 0, 0x800000)

    def test_reports_4k_erase_extent(self):
        extent = release.validate_write_extent(
            "app.bin", 0x10000, 0x1234, 0x10000, 0x310000
        )
        self.assertEqual((0x10000, 0x12000), extent)

    def test_rejects_changed_fixed_partition_map(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            changed = list(EXPECTED_PARTITIONS)
            changed[5] = ("cardid", 0x01, 0x02, 0x355000, 0x004000)
            (build / "partition_table" / "partition-table.bin").write_bytes(
                partition_table(changed)
            )
            with self.assertRaisesRegex(RuntimeError, "cardid"):
                release.validate_release_inputs(build, "trae_card.bin")

    def test_rejects_missing_corrupt_and_mismatched_pet_art(self):
        for damage in ("missing", "corrupt", "wrong-app", "wrong-catalog", "oversize"):
            with self.subTest(damage=damage), tempfile.TemporaryDirectory() as tmp:
                build = write_fixture(tmp)
                path = build / "petart.bin"
                if damage == "missing": path.unlink()
                elif damage == "corrupt":
                    data = bytearray(path.read_bytes()); data[-1] ^= 1; path.write_bytes(data)
                elif damage == "wrong-app": (build / "trae_card.bin").write_bytes(b"\xe9wrong app")
                elif damage == "wrong-catalog": (build / "petart_catalog.json").write_text('{}')
                elif damage == "oversize": path.write_bytes(b"PTP1" + b"X" * 0x100000)
                with self.assertRaisesRegex(RuntimeError, "petart|PETART"):
                    release.validate_release_inputs(build, "trae_card.bin")

    def test_rejects_valid_pet_payload_in_new_pet_free_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            raw = (build / "petart.bin").read_bytes()
            catalog = json.loads((build / "petart_catalog.json").read_text())
            records = pet_art_pack.validate_pack(raw, catalog)
            entries = [(r['symbol'], r['width'], r['height'], raw[r['offset']:r['offset']+r['bytes']]) for r in records]
            entries.append(('pet_icon_gift', *entries[0][1:]))
            data, catalog = pet_art_pack.build_pack(entries)
            (build / "petart.bin").write_bytes(data)
            (build / "petart_catalog.json").write_text(json.dumps(catalog))
            app = build / "trae_card.bin"
            app.write_bytes(app.read_bytes() + pet_art_pack.fingerprint(data).encode() + b'\0')
            with self.assertRaisesRegex(RuntimeError, 'shared game'):
                release.validate_release_inputs(build, 'trae_card.bin')

    def test_archive_rejects_missing_shared_metadata_and_pet_free_downgrade(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); build = write_fixture(root / 'build')
            archive = root / 'release.zip'
            release.write_release_artifacts(build, archive, root / 'app.bin', 'trae_card.bin', dict(
                version='test', target='esp32c3', flash_size='8MB', periph='default',
                branch='test', channel='beta', git='test', status='intermediate'))
            with zipfile.ZipFile(archive) as z:
                payload = {name: z.read(name) for name in z.namelist()}
            for damage in ('removed-metadata', 'legacy-metadata', 'wrong-uses', 'missing-app-requirement'):
                with self.subTest(damage=damage):
                    changed = dict(payload); manifest = json.loads(payload['firmware-manifest.json'])
                    if damage == 'removed-metadata': del manifest['shared_game_art']
                    elif damage == 'legacy-metadata': manifest['pet_art'] = manifest.pop('shared_game_art')
                    elif damage == 'wrong-uses': manifest['shared_game_art']['uses'] = ['pet']
                    else: del manifest['standalone_application']['requires_matching_shared_game_art']
                    changed['firmware-manifest.json'] = json.dumps(manifest).encode()
                    changed['SHA256SUMS'] = ''.join(f'{hashlib.sha256(data).hexdigest()}  {name}\n'
                        for name, data in sorted(changed.items()) if name != 'SHA256SUMS').encode()
                    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
                        for name, data in changed.items(): z.writestr(name, data)
                    with self.assertRaises(RuntimeError): verify_release.verify_archive(archive)

    def test_welcome_prompt_matches_application_and_notice(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp); build = write_fixture(root / 'build')
            app = build / 'trae_card.bin'
            prompt = (release.ROOT / 'components/platform/xiaozhi/welcome_zh.pcm').read_bytes()
            app.write_bytes(app.read_bytes()+prompt)
            archive = root / 'release.zip'
            release.write_release_artifacts(build, archive, root / 'app.bin', 'trae_card.bin', dict(
                version='test', target='esp32c3', flash_size='8MB', periph='default',
                branch='test', channel='beta', git='test', status='intermediate'))
            manifest = verify_release.verify_archive(archive,build)
            self.assertEqual(manifest['welcome_audio']['sha256'],hashlib.sha256(prompt).hexdigest())
            self.assertEqual(manifest['welcome_audio']['format'], 'PCM-S16LE')
            self.assertEqual(manifest['welcome_audio']['sample_rate'], 24000)
            with zipfile.ZipFile(archive) as z:
                original={name:z.read(name) for name in z.namelist()}
            for damage in ('offset','length','sha','notice'):
                with self.subTest(damage=damage):
                    payload=dict(original); data=json.loads(original['firmware-manifest.json'])
                    entry=data['welcome_audio']
                    if damage=='offset': entry['app_offset']-=1
                    elif damage=='length': entry['bytes']+=1
                    elif damage=='sha': entry['sha256']='0'*64
                    else: del payload[entry['notice']]
                    payload['firmware-manifest.json']=json.dumps(data).encode()
                    payload['SHA256SUMS']=''.join(
                        f'{hashlib.sha256(v).hexdigest()}  {k}\n' for k,v in sorted(payload.items()) if k!='SHA256SUMS').encode()
                    with zipfile.ZipFile(archive,'w') as z:
                        for name,value in payload.items():z.writestr(name,value)
                    with self.assertRaisesRegex(RuntimeError,'welcome_audio'):verify_release.verify_archive(archive)

    def test_pet_art_erase_extent_preserves_all_existing_partitions(self):
        with tempfile.TemporaryDirectory() as tmp:
            segments = release.validate_release_inputs(write_fixture(tmp), "trae_card.bin")
            pet = segments[4]
            self.assertEqual(0x500000, pet.erase_start)
            self.assertLessEqual(pet.erase_end, 0x5e9000)
            for name, _, _, start, size in EXPECTED_PARTITIONS[:9]:
                self.assertFalse(pet.erase_start < start + size and pet.erase_end > start, name)

    def test_rejects_pet_art_partition_without_readonly_flag(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = write_fixture(tmp)
            path = build / "partition_table/partition-table.bin"
            index = next(i for i, row in enumerate(EXPECTED_PARTITIONS) if row[0] == 'petart')
            data = bytearray(path.read_bytes()); struct.pack_into('<I', data, index*32+28, 0); path.write_bytes(data)
            with self.assertRaisesRegex(RuntimeError, "flags"):
                release.validate_release_inputs(build, "trae_card.bin")

    def test_real_zip_is_flat_and_hashes_match_build_inputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            build = write_fixture(base / "build")
            archive = base / "release.zip"
            standalone = base / "release-app-0x10000.bin"
            release.write_release_artifacts(
                build,
                archive,
                standalone,
                app_bin="trae_card.bin",
                metadata={
                    "version": "1.2.0",
                    "target": "esp32c3",
                    "flash_size": "8MB",
                    "periph": "disp-aud-btn-ble-bat",
                    "branch": "test",
                    "channel": "beta",
                    "git": "abcdef0+dirty",
                    "status": "intermediate",
                },
            )

            verified = verify_release.verify_archive(archive, build, standalone)
            self.assertEqual("intermediate", verified["status"])
            self.assertEqual((build / "trae_card.bin").read_bytes(), standalone.read_bytes())
            with zipfile.ZipFile(archive) as package:
                names = package.namelist()
                self.assertTrue(all("/" not in name for name in names))
                self.assertNotIn("merged-firmware.bin", names)
                required = {
                    "bootloader.bin", "partition-table.bin", "trae_card.bin",
                    "imgava.bin", "petart.bin", "petart_catalog.json", "avatar_catalog.json", "flash_args",
                    "flasher_args.json", "firmware-manifest.json", "SHA256SUMS",
                    "MANIFEST.txt",
                } | set(release.FONT_METADATA_FILES) | set(release.LIBRARY_LICENSE_FILES)
                self.assertTrue(required.issubset(names))
                manifest = json.loads(package.read("firmware-manifest.json"))
                self.assertEqual("intermediate", manifest["status"])
                self.assertEqual({"pet": False}, manifest["features"])
                self.assertNotIn("pet_art", manifest)
                self.assertNotIn("requires_matching_pet_art", manifest["standalone_application"])
                self.assertEqual(["dodge", "home-avatar"], manifest["shared_game_art"]["uses"])
                self.assertEqual(
                    [0, 0x8000, 0x10000, 0x3FA000, 0x500000, 0x600000],
                    [item["offset"] for item in manifest["segments"]],
                )
                self.assertEqual(
                    ["bootloader.bin", "partition-table.bin", "trae_card.bin", "imgava.bin", "petart.bin", "companion.bin"],
                    [item["file"] for item in manifest["segments"]],
                )
                flat_args = json.loads(package.read("flasher_args.json"))
                self.assertEqual({
                    "0x0": "bootloader.bin",
                    "0x8000": "partition-table.bin",
                    "0x10000": "trae_card.bin",
                    "0x3fa000": "imgava.bin",
            "0x500000": "petart.bin",
            "0x600000": "companion.bin",
                }, flat_args["flash_files"])
                sums = {}
                for line in package.read("SHA256SUMS").decode().splitlines():
                    digest, name = line.split("  ", 1)
                    sums[name] = digest
                for name in required - {"SHA256SUMS", "MANIFEST.txt"}:
                    self.assertEqual(hashlib.sha256(package.read(name)).hexdigest(), sums[name])
                for item in manifest["segments"]:
                    self.assertEqual(hashlib.sha256(package.read(item["file"])).hexdigest(),
                                     item["sha256"])
                    self.assertFalse(
                        item["erase_start"] < 0x35A000 and item["erase_end"] > 0x356000
                    )


if __name__ == "__main__":
    unittest.main()
