"""Keep lifecycle code byte-for-byte from firmware, excluding only HW decoding."""
import json
from pathlib import Path
import sys


def extract(source: Path) -> str:
    text = source.read_text()
    start = text.index('static const char *TAG = "jpeg_view";')
    state_end = text.index('// 块解码输出条带缓冲', start)
    decoding = text.index('static volatile bool s_decoding = false;', state_end)
    decoding_end = text.index('\n', decoding) + 1
    functions = text.index('static int build_dsc(void) {', decoding_end)
    end = text.rindex('#endif // PERIPH_DISPLAY')
    blocks = [(start, state_end), (decoding, decoding_end), (functions, end)]
    result = ['/* Generated: exact production state and lifecycle functions. */\n']
    for a, b in blocks:
        result.append(f'#line {text.count(chr(10), 0, a) + 1} {json.dumps(str(source))}\n')
        result.append(text[a:b])
    return ''.join(result)


if __name__ == '__main__':
    source, destination = map(Path, sys.argv[1:])
    destination.write_text(extract(source))
