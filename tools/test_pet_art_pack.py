import json
import importlib.util
from types import SimpleNamespace
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from tools import pet_art_pack as pack


def sprite(width=2, height=2):
    return (b'PSR1\x01\0\0\0' + bytes((10, 20, 30, 255)) +
            struct.pack('<' + 'H' * (height + 1), *(2*i for i in range(height + 1))) +
            bytes((0x80 | (width-1), 0)) * height)


class PetArtPackTest(unittest.TestCase):
    def test_round_trip_is_canonical_across_input_order(self):
        entries = [('bobo_one', 2, 2, sprite()), ('bobo_two', 2, 2, sprite())]
        data, catalog = pack.build_pack(entries)
        self.assertEqual((data, catalog), pack.build_pack(list(reversed(entries))))
        self.assertEqual(2, len(pack.validate_pack(data, catalog)))
        self.assertLess(len(data), pack.CAPACITY)

    def test_payload_crc_and_truncation(self):
        data, catalog = pack.build_pack([('bobo_one', 2, 2, sprite())])
        for broken in (data[:-1], data + b'\0', data[:2], b'\xff' * len(data)):
            with self.assertRaises(ValueError): pack.validate_pack(broken, catalog)
        broken = bytearray(data); broken[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC'): pack.validate_pack(broken, catalog)

    def test_valid_crc_does_not_hide_bad_id_dimension_or_offset(self):
        data, catalog = pack.build_pack([('bobo_one', 2, 2, sprite())])
        for offset, value, fmt in ((64, 0, '<I'), (68, 0, '<H'), (70, 321, '<H'),
                                  (72, 0xffffffff, '<I'), (76, 0xffffffff, '<I')):
            broken = bytearray(data); struct.pack_into(fmt, broken, offset, value)
            struct.pack_into('<I', broken, 28, zlib.crc32(broken[64:88]))
            with self.assertRaises(ValueError): pack.validate_pack(broken, catalog)

    def test_duplicate_id_bad_dimensions_and_capacity_rejected(self):
        with self.assertRaises(ValueError): pack.build_pack([('same', 2, 2, sprite())]*2)
        for width, height in ((0, 2), (241, 2), (2, 321)):
            with self.assertRaises(ValueError): pack.build_pack([('one', width, height, sprite())])
        with self.assertRaises(ValueError): pack.build_pack([('one', 2, 2, b'x'*pack.CAPACITY)])

    def test_reference_mapping_covers_every_symbol_and_manifest_entry(self):
        from PIL import Image
        root = Path(__file__).resolve().parents[1]
        spec = importlib.util.spec_from_file_location("pet_references", root/"tests/host/pixel_sprite/prepare_references.py")
        references = importlib.util.module_from_spec(spec); spec.loader.exec_module(references)
        symbols = ("bobo_art_wave", "dodge_art_books", "pet_gift_0", "pet_sunflower_baby_home_idle")
        with tempfile.TemporaryDirectory() as tmp:
            generated = Path(tmp)
            catalog = pack.write_pet_art_assets(generated, [
                (symbol, SimpleNamespace(width=2, height=2), sprite()) for symbol in symbols])
            assets = generated/"assets/bobo"; assets.mkdir()
            items = []
            for asset in catalog["sprites"]:
                name = references.png_name(asset["symbol"])
                Image.new("RGBA", (2, 2), (30, 20, 10, 255)).save(assets/f"{name}.png")
                items.append(dict(id=name, format="PSR1-64", **{
                    key: asset[key] for key in ("width", "height", "bytes", "sha256")}))
            manifest = generated/"assets/pixel-assets.json"
            manifest.write_text(json.dumps({"sprites": items}))
            output = generated/"references"
            result = references.prepare_references(assets, output)
            self.assertEqual(set(symbols), {item["symbol"] for item in result})
            self.assertEqual({"wave.png", "dodge_books.png", "pet_gift_0.png", "pet_sunflower_baby_home_idle.png"},
                             {item["png"] for item in result})
            for item in result:
                self.assertEqual(f"{item['asset_id']:08x}.rgba", item["rgba"])
                self.assertEqual(bytes((30, 20, 10, 255))*4, (output/item["rgba"]).read_bytes())
            manifest.write_text(json.dumps({"sprites": items[:-1]}))
            with self.assertRaisesRegex(ValueError, "counts"):
                references.prepare_references(assets, output)
            manifest.write_text(json.dumps({"sprites": items+[items[0]]}))
            with self.assertRaisesRegex(ValueError, "duplicate"):
                references.prepare_references(assets, output)

    def test_shared_assets_filter_preserves_only_home_avatar_and_dodge(self):
        symbols = ('bobo_art_wave', 'bobo_art_walk_left', 'bobo_art_walk_right',
                   'dodge_art_books', 'dodge_art_luggage', 'dodge_art_robot', 'dodge_art_plant')
        source, catalog = pack.build_pack([(name, 2, 2, sprite()) for name in (*symbols, 'pet_icon_gift')])
        data, filtered = pack.shared_game_pack(source, catalog)
        self.assertEqual(set(symbols), {r['symbol'] for r in pack.validate_pack(data, filtered)})
        self.assertLess(len(data), len(source))
        for retained in filtered['sprites']:
            original = next(r for r in catalog['sprites'] if r['symbol'] == retained['symbol'])
            self.assertEqual(original['sha256'], retained['sha256'])
        broken = bytearray(source); broken[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'CRC'):
            pack.shared_game_pack(broken, catalog)
        missing, missing_catalog = pack.build_pack([(name, 2, 2, sprite()) for name in symbols[:-1]])
        with self.assertRaisesRegex(ValueError, 'missing shared'):
            pack.shared_game_pack(missing, missing_catalog)

    def test_current_pack_catalog_matches_generated_app_and_sources(self):
        root = Path(__file__).resolve().parents[1]
        data = (root/'assets/petart.bin').read_bytes()
        catalog = json.loads((root/'assets/petart_catalog.json').read_text())
        entries = pack.validate_pack(data, catalog)
        self.assertEqual(pack.SHARED_GAME_SYMBOLS, {r['symbol'] for r in entries})
        generated = (root/'components/ui/presentation/src/bobo_art.c').read_text()
        self.assertIn(pack.fingerprint(data), generated)
        self.assertNotIn('_pixels[]', generated)
        self.assertIn("size_t bobo_art_asset_count(void)", generated)
        self.assertIn("const pet_art_asset_t *bobo_art_asset(size_t index)", generated)
        for item in entries: self.assertIn(item['symbol'], generated)


if __name__ == '__main__': unittest.main()
