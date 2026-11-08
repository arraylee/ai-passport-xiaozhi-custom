"""Replay an externally supplied official Recovery checkout against real firmware images.

Requires a C compiler plus CommonCrypto (macOS) or OpenSSL (Linux); the IDF
partition-table generator is taken from IDF_PATH. No device or network is used.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name, help_text in [('installer', 'Upstream ai-passport-miniapp-installer checkout'),
                            ('image', 'Final merged XiaoZhi BIN'), ('app', 'Matching ESP-IDF application BIN'),
                            ('previous', 'Previous merged .17 BIN (negative control)'),
                            ('result', 'Output simulated flash image')]:
        parser.add_argument('--'+name, type=Path, required=True, help=help_text)
    args = parser.parse_args()
    sys.path.insert(0, str(HERE.parents[1] / 'tools'))
    from pack_miniapp import verify_esp
    verify_esp(args.app.read_bytes())
    main_dir = args.installer.resolve() / 'recovery/main'
    source = (main_dir / 'wifi_install.c').read_text()
    with tempfile.TemporaryDirectory() as path:
        tmp = Path(path)
        (tmp/'recovery_download.inc').write_text(source[source.index('static bool table_md5('):
                                                         source.index('static void worker(')])
        table = tmp / 'installer-table.bin'
        subprocess.run([sys.executable, str(Path(os.environ['IDF_PATH'])/'components/partition_table/gen_esp32part.py'),
                        str(args.installer/'partitions.csv'), str(table)], check=True)
        exe = tmp / 'replay'
        subprocess.run(['cc','-std=c11','-O2','-Wno-deprecated-declarations','-I',str(tmp),'-I',str(main_dir),
                        str(HERE/'replay.c'),str(main_dir/'install_layout.c'),
                        *([] if sys.platform=='darwin' else ['-lcrypto']),'-o',str(exe)], check=True)
        subprocess.run([str(exe),str(args.image),str(args.app),str(table),str(args.previous),str(args.result)],check=True)


if __name__=='__main__':
    main()
