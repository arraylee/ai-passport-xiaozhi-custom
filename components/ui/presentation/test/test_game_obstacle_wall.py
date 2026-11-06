"""The old neon-wall contract is replaced by native sprite bounds and Flash cost."""
from pathlib import Path
import json
import unittest
from PIL import Image

ROOT = Path(__file__).resolve().parents[4]

class GameArtworkContractTest(unittest.TestCase):
    def test_four_props_fit_the_native_lane_and_bounded_flash(self):
        manifest = json.loads((ROOT / 'assets/pixel-assets.json').read_text())
        sprites = {item['id']: item for item in manifest['sprites']}
        for name in ('luggage', 'robot', 'plant', 'books'):
            item = sprites['dodge_' + name]
            with Image.open(ROOT / ('assets/bobo/dodge_' + name + '.png')) as art:
                self.assertEqual((48, 64), art.size)
                self.assertEqual('RGBA', art.mode)
                self.assertTrue(art.getchannel('A').getextrema()[0] == 0)
            self.assertEqual('PSR1-64', item['format'])
            self.assertLess(item['bytes'], 1024 + 48 * 64)
        # The sprite payload moved out of the 3 MiB app into a dedicated
        # 1 MiB read-only pack when collectible companions were added.
        catalog = json.loads((ROOT / 'assets/petart_catalog.json').read_text())
        self.assertEqual(manifest['sprite_flash_bytes'], catalog['sprite_bytes'])
        self.assertLessEqual(catalog['bytes'], 0x100000)

    def test_walking_bobo_fits_native_size_without_a_canvas_buffer(self):
        for name in ('walk_left', 'walk_right'):
            with Image.open(ROOT / ('assets/bobo/' + name + '.png')) as art:
                self.assertEqual((40, 40), art.size)
        source = (ROOT / 'components/ui/presentation/src/ui_game.c').read_text()
        self.assertNotIn('lv_canvas_create', source)
        self.assertIn('lv_obj_set_style_clip_corner(G.screen, false, 0)', source)

if __name__ == '__main__':
    unittest.main()
