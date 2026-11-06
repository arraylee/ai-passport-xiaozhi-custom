import unittest,struct,zlib,tempfile,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).parent))
import companion_pack as p
class CompanionPackTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.path=Path(cls.temp.name);p.build(cls.path);cls.blob=(cls.path/'companion.bin').read_bytes()
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def test_valid_and_native(self):
        entries=p.validate(self.blob);self.assertEqual(len(entries),14)
        self.assertTrue(all(n==144*144*3 for _,n in entries[2:]));self.assertLess(len(self.blob),0x200001)
    def test_corruption(self):
        for pos in [0,4,8,12,16,128,len(self.blob)-0x100000]:
            data=bytearray(self.blob);data[pos]^=255
            with self.assertRaises(ValueError):p.validate(data)
    def test_range_even_with_new_crc(self):
        data=bytearray(self.blob);struct.pack_into('<I',data,16,0xfffffff0)
        total=struct.unpack_from('<I',data,8)[0];struct.pack_into('<I',data,12,zlib.crc32(data[16:total]))
        with self.assertRaises(ValueError):p.validate(data)
    def test_external_fonts_not_duplicated(self):
        body=(self.path/'lv_font_cn_16.c').read_text();title=(self.path/'lv_font_cn_24.c').read_text()
        self.assertNotIn('passport_body_glyph_bitmap[]',body);self.assertIn('passport_bind_body',body)
        self.assertNotIn('passport_nickname_compressed[]',title);self.assertIn('passport_bind_nickname',title)
if __name__=='__main__':unittest.main()
