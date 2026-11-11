import os
from pathlib import Path
import json
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class OnlineToolsTest(unittest.TestCase):
    def test_voice_weather_and_radio(self):
        sdk = os.environ.get("IDF_PATH") or json.loads(
            (ROOT / "build-miniapp/project_description.json").read_text())["idf_path"]
        cjson = Path(sdk) / "components/json/cJSON"
        module = ROOT / "components/platform/xiaozhi"
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "online"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
                            "-fno-omit-frame-pointer", "-fsanitize=address,undefined",
                            *["-I"+str(p) for p in [module, module/"include", cjson, ROOT/"components/core/ports/include"]],
                            str(Path(__file__).with_name("test_online_tools.c")),
                            *[str(module/f) for f in ("xz_weather.c", "xz_stations.c", "xz_mcp.c")],
                            str(cjson/"cJSON.c"), "-o", str(exe)], check=True)
            live = os.environ.get("PASSPORT_ONLINE_FIXTURES")
            subprocess.run([str(exe)]+([live] if live else []), check=True, timeout=10)

