"""Real cJSON/Unity ASan + UBSan regressions for JSON and RTTTL inputs.

Run with: python3 tests/host/run_input_boundaries.py
Uses IDF_PATH, or the SDK recorded by the existing firmware build. No device,
network, fake JSON parser, or fake assertions are required. Artifacts are /tmp.
"""
from pathlib import Path
import json
import os
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    idf = os.environ.get("IDF_PATH")
    if not idf:
        description = ROOT / "build/project_description.json"
        if not description.exists():
            raise SystemExit("Set IDF_PATH or build firmware once to locate ESP-IDF.")
        idf = json.loads(description.read_text())["idf_path"]
    sdk = Path(idf)
    unity = sdk / "components/unity/unity/src"
    cjson = sdk / "components/json/cJSON"
    services = ROOT / "components/core/services"
    command = [
        *shlex.split(os.environ.get("CC", "cc")), "-std=c11", "-g", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined,float-cast-overflow", "-fno-sanitize-recover=all",
        "-fno-omit-frame-pointer", "-UNDEBUG", "-I" + str(services / "include"),
        "-I" + str(cjson), "-I" + str(unity),
    ]
    production = [services / "src" / name for name in ("config_json.c", "rtttl.c", "profile_ctl.c")]
    environment = dict(os.environ)
    # LeakSanitizer cannot inspect threads under the host sandbox's ptrace.
    # Address/UB/float-cast checks remain active and abort at the first error.
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    with tempfile.TemporaryDirectory(prefix="passport-input-", dir="/tmp") as directory:
        temp = Path(directory)
        probe = temp / "probes"
        subprocess.run([
            *command, str(ROOT / "tests/host/test_input_boundaries.c"),
            *map(str, production), str(cjson / "cJSON.c"), "-lm", "-o", str(probe)
        ], check=True)
        for case in ("utf8", "number", "time", "integer", "octave", "mutations"):
            subprocess.run([str(probe), case], check=True, env=environment)

        # Only replace ESP-IDF's TEST_CASE registration. Assertions and all
        # existing/new test bodies execute unchanged against real Unity.
        generated, functions = [], []
        for filename in ("test_config_json.c", "test_rtttl.c"):
            test = services / "test" / filename

            def replace(match):
                name = f"input_test_{len(functions)}"
                functions.append((name, match.group(1)))
                return "void " + name + "(void)"

            source = re.sub(r'TEST_CASE\("([^"]+)",\s*"[^"]*"\)', replace, test.read_text())
            output = temp / filename
            output.write_text(source)
            generated.append(output)
        runner = temp / "main.c"
        runner.write_text(
            '#include "unity.h"\nvoid setUp(void){}\nvoid tearDown(void){}\n'
            + "".join(f"void {name}(void);\n" for name, _ in functions)
            + 'int main(void){UnityBegin("sanitized inputs");\n'
            + "".join(f"UnityDefaultTestRun({name},{json.dumps(title)},__LINE__);\n" for name, title in functions)
            + "return UnityEnd();}\n"
        )
        executable = temp / "unity_inputs"
        subprocess.run([
            *command, *map(str, generated), *map(str, production), str(runner),
            str(unity / "unity.c"), str(cjson / "cJSON.c"), "-lm", "-o", str(executable)
        ], check=True)
        subprocess.run([str(executable)], check=True, env=environment)


if __name__ == "__main__":
    main()
