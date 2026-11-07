"""Run the production MCP, theme reader, stream writer and asset decoders under sanitizers."""
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
COMPONENT = ROOT / "components/platform/xiaozhi"


def synthetic_pack():
    # Same table and prefix as the upstream packer. The artwork is also used by the device.
    index = {"version": 1, "chip_model": "esp32c3",
             "display_config": {"width": 240, "height": 320},
             "emoji_collection": [{"name": "happy", "file": "happy.png"}]}
    files = {"index.json": json.dumps(index).encode(),
             "happy.png": (ROOT / "assets/xiaozhi/official/happy.png").read_bytes()}
    table = bytearray()
    contents = bytearray()
    for name, value in files.items():
        table.extend(struct.pack("<32sIIHH", name.encode(), len(value), len(contents), 0, 0))
        contents.extend(b"ZZ" + value)
    body = table + contents
    return struct.pack("<III", len(files), sum(body) & 65535, len(body)) + body


class OfficialTheme(unittest.TestCase):
    def test_theme_and_voice_controls(self):
        sdk = os.environ.get("IDF_PATH")
        if not sdk:
            sdk = json.loads((ROOT / "build/project_description.json").read_text())["idf_path"]
        cjson = Path(sdk) / "components/json/cJSON"
        puff = ROOT / "components/ui/presentation/vendor/puff"
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            fixture = folder / "theme.bin"
            fixture.write_bytes(synthetic_pack())
            binary = folder / "official"
            modules = ["xz_theme", "xz_theme_font", "xz_theme_transfer", "xz_theme_image",
                       "xz_mcp", "xz_protocol"]
            dirs = [COMPONENT, COMPONENT / "include", ROOT / "components/core/ports/include", puff, cjson]
            subprocess.run(["cc", "-std=c11", "-g", "-Wno-deprecated-declarations",
                            "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                            *["-I" + str(d) for d in dirs],
                            str(ROOT / "tests/host/test_official_pack.c"),
                            *[str(COMPONENT / (s + ".c")) for s in modules],
                            str(puff / "puff.c"), str(cjson / "cJSON.c"), "-lz", "-o", str(binary)],
                           check=True)
            samples = os.environ.get("PASSPORT_THEME_FIXTURES", "")
            args = [name for name in samples.split(os.pathsep) if name]
            subprocess.run([str(binary), str(fixture), *args], check=True, timeout=45)

