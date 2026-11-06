import contextlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
from tools import body_font_pack as b, companion_pack, gen_cn_font as g, gen_font_oracle


class BodyFontPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.path = Path(cls.temp.name)
        with contextlib.redirect_stdout(io.StringIO()): companion_pack.build(cls.path)
        cls.font = (cls.path/'body_font.bin').read_bytes()
        cls.sprites = (g.ROOT/'assets/petart.bin').read_bytes()
        cls.catalog = json.loads((g.ROOT/'assets/petart_catalog.json').read_text())

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_native_pixel_oracle_is_current(self):
        with contextlib.redirect_stdout(io.StringIO()): gen_font_oracle.check()

    def test_metrics_are_exact_compact_native_values(self):
        h = b.validate(self.font)
        source = (g.OUT_DIR/'lv_font_cn_16.c').read_text()
        import re
        rows = re.findall(r'\{([^{}]*)\}', g.array_data(source)['passport_body_glyph_dsc'][1])
        self.assertEqual(len(rows), h[7])
        for i, row in enumerate(rows):
            m = {k:int(v) for k,v in re.findall(r'\.(\w+)\s*=\s*(-?\d+)',row)}
            word,w,height,x,y = struct.unpack_from('<IBBbb',self.font,h[6]+i*8)
            self.assertEqual((word&0xfffff,word>>20,w,height,x,y),
                tuple(m[k] for k in ('bitmap_index','adv_w','box_w','box_h','ofs_x','ofs_y')))

    def test_bundle_preserves_sprite_bytes_and_partition_bounds(self):
        data, catalog = b.bundle(self.sprites,self.catalog,self.font)
        sprites,font = b.split(data,catalog)
        self.assertEqual(sprites,self.sprites);self.assertEqual(font,self.font)
        self.assertLessEqual(len(data),0x100000)
        self.assertEqual(data[0x10000:0x10004],b'BDF1')
        self.assertEqual(catalog['sha256'],self.catalog['sha256'])

    def test_legacy_sprite_only_compatibility(self):
        self.assertEqual(b.split(self.sprites,self.catalog),(self.sprites,None))

    def test_corruption_and_truncation_rejected(self):
        for at in [0,4,8,12,16,24,32,36,64,len(self.font)-1]:
            data=bytearray(self.font);data[at]^=255
            with self.assertRaises(ValueError):b.validate(data)
        with self.assertRaises(ValueError):b.validate(self.font[:-1])

    def test_invalid_geometry_even_with_valid_crc(self):
        for at,value in [(16,65),(24,0xfffffffc),(28,0),(32,3),(36,64),(52,1)]:
            data=bytearray(self.font);struct.pack_into('<I',data,at,value)
            struct.pack_into('<I',data,12,zlib.crc32(data[16:]))
            with self.assertRaises(ValueError):b.validate(data)

    def test_bundle_padding_and_catalog_tampering_rejected(self):
        data,catalog=b.bundle(self.sprites,self.catalog,self.font)
        bad=bytearray(data);bad[len(self.sprites)]=0
        with self.assertRaises(ValueError):b.split(bad,catalog)
        catalog['body_font']['sha256']='0'*64
        with self.assertRaises(ValueError):b.split(data,catalog)

    def test_mapped_font_does_not_duplicate_pixels_or_metrics(self):
        code=(self.path/'lv_font_cn_16.c').read_text()
        for name in ['passport_body_glyph_bitmap[]','passport_body_glyph_dsc[]','passport_body_unicode_list_0[]']:
            self.assertNotIn(name,code)
        self.assertIn('passport_body_dsc.glyph_dsc &&',code)

    def test_release_pairs_body_font_and_companion_with_application(self):
        from scripts import release
        from tools import pet_art_pack
        data,catalog=b.bundle(self.sprites,self.catalog,self.font)
        app=(pet_art_pack.fingerprint(self.sprites)+'\0'+b.fingerprint(self.font)+'\0').encode()
        release.validate_pet_art_payload(data,json.dumps(catalog).encode(),app)
        with self.assertRaisesRegex(RuntimeError,'body font'):
            release.validate_pet_art_payload(data,json.dumps(catalog).encode(),b'bad app\0'+app.split(b'BODYFONT')[0])
        with self.assertRaisesRegex(RuntimeError,'body font'):
            release.validate_pet_art_payload(self.sprites,json.dumps(self.catalog).encode(),app)
        companion=(self.path/'companion.bin').read_bytes()
        used=struct.unpack_from('<I',companion,8)[0]
        import hashlib
        app+=('COMPANION-SHA256:'+hashlib.sha256(companion[:used]).hexdigest()+'\0').encode()
        release.validate_companion_payload(companion,app)
        with self.assertRaisesRegex(RuntimeError,'body font'):
            release.validate_companion_payload(companion,app.replace(b'BODYFONT-SHA256:',b'BODYFONT-BROKEN:'))


if __name__=='__main__':unittest.main()
