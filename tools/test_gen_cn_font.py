import contextlib
import io
import unittest

from tools.gen_cn_font import (
    NICKNAME_FALLBACK_CHARS,
    check,
    gb2312_hanzi,
    nickname_hanzi,
    historical_sets,
    DIALOG_EXTRA_CHARS,
    split_sparse_cmap_chars,
)


class ChineseFontCharsetTest(unittest.TestCase):
    def test_shipped_fallbacks_match_current_ui_and_cover_profile_characters(self):
        # UI copy changes can remove a formerly common glyph from the fast
        # subset. Check the shipped C maps, not only the generator's input list.
        with contextlib.redirect_stdout(io.StringIO()):
            check()

    def test_level1_keeps_small_font_charset(self):
        self.assertEqual(3755, len(gb2312_hanzi()))

    def test_both_dynamic_sizes_cover_all_hanzi_and_dialogue(self):
        sets = historical_sets()
        self.assertEqual(sets[16], sets[24])
        self.assertTrue(set(map(ord, gb2312_hanzi(True))).issubset(sets[16]))
        self.assertTrue(set(map(ord, "哦呀啊嗯呐呦哟嗨嘻嘿唉诶噢喔哇嘞咯嘟" + DIALOG_EXTRA_CHARS)).issubset(sets[16]))

    def test_full_charset_contains_level2_name_characters(self):
        chars = gb2312_hanzi(include_level2=True)
        self.assertEqual(6763, len(chars))
        for char in "阚邴郗逯仉佘亓訾琰珩":
            self.assertIn(char, chars)

    def test_nickname_charset_contains_non_gb2312_name_characters(self):
        chars = nickname_hanzi()
        self.assertEqual(6907, len(chars))
        for char in "珺鍠雲飂鎛琭汎佈柷敔吥昇嗏燚溦彧燊吳垚榮拝楿髙。、～！（）°μ…″":
            self.assertIn(char, chars)

    def test_requested_nickname_glyphs_are_included(self):
        chars = nickname_hanzi()
        requested = (
            "喆鋆韡烋烜璟旻祎姞飔玥瑄崝珅翬珹悅炘赟贇渱璠珮劼芃甦镕爔昪禤暢玶淏崟"
            "𤩽熺偲竑暐棨钖熇珽翀嫄宬湲湧嫚濛堃啟愷旸㛃倞玙杍翚桪昫昞樑纮澔梽頔"
            "姮冮顥晞沄屾祐锜瑨焜曈詝飏浛瑀栯橦妘競琎敭鹍镠禕乂荍珵嶒辀昳軻璘棫"
            "皛麥璕漘栢奡"
        )
        self.assertEqual(106, len(requested))
        self.assertTrue(set(requested).issubset(chars))
        self.assertEqual(set("•¯𤩽"), set(NICKNAME_FALLBACK_CHARS))

    def test_registration_list_text_symbols_are_included(self):
        chars = nickname_hanzi()
        self.assertTrue(set("·，•′¯").issubset(chars))
        self.assertTrue(set("🍊🇦🇷💤🦋🌀🤪🍐🌙🪵🍙🇨🇳🐟").isdisjoint(chars))

    def test_sparse_cmaps_fit_lvgl_uint16_offsets(self):
        groups = split_sparse_cmap_chars(sorted(nickname_hanzi(), key=ord))
        self.assertEqual(2, len(groups))
        self.assertEqual(["𤩽"], groups[-1])
        for group in groups:
            self.assertLessEqual(ord(group[-1]) - ord(group[0]), 0xFFFF)


if __name__ == "__main__":
    unittest.main()
