"""Source contracts for filtering unsupported nickname glyphs before display."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[4]
PROFILE_SOURCE = ROOT / "components/ui/presentation/src/ui_profile.c"


def function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^;]*?\)\s*\{{", source, re.DOTALL)
    if match is None:
        raise AssertionError(f"function {name} is missing")
    brace = source.find("{", match.start())
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"function {name} has no closing brace")


class ProfileNicknameFilterContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = PROFILE_SOURCE.read_text(encoding="utf-8")

    def test_filter_decodes_whole_utf8_codepoints_and_queries_real_font(self) -> None:
        body = function_body(self.source, "nickname_display_text")
        self.assertIn("nickname_utf8_next(name, &read, &letter)", body)
        self.assertIn("lv_font_get_glyph_dsc(&ui_font_title", body)
        self.assertIn("if (nickname_is_han(letter)) out[write++] = '*'", body)
        self.assertIn("memcpy(out + write, name + start, encoded_len)", body)

    def test_han_detection_covers_basic_compatibility_and_extension_blocks(self) -> None:
        body = function_body(self.source, "nickname_is_han")
        for boundary in ("0x3400", "0x4DBF", "0x4E00", "0x9FFF",
                         "0xF900", "0xFAFF", "0x20000", "0x323AF"):
            self.assertIn(boundary, body)

    def test_profile_setter_displays_filtered_copy(self) -> None:
        body = function_body(self.source, "ui_profile_set_name")
        self.assertIn("nickname_display_text(name, display)", body)
        self.assertIn("lv_label_set_text(P.name_lbl, display)", body)
        self.assertNotIn("lv_label_set_text(P.name_lbl, name)", body)


if __name__ == "__main__":
    unittest.main(verbosity=2)
