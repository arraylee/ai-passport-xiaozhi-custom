"""Exercise the real codec suspend write/readback against register boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define ESP_LOGE(...) ((void)0)
typedef struct audio_codec_ctrl_if_t audio_codec_ctrl_if_t;
struct audio_codec_ctrl_if_t {
    int (*write_reg)(const audio_codec_ctrl_if_t *, int, int, const void *, int);
    int (*read_reg)(const audio_codec_ctrl_if_t *, int, int, void *, int);
};
typedef struct { const audio_codec_ctrl_if_t *ctrl_if; } es_ctx_t;
static uint8_t regs[256], reg0e=0x7f;
static int corrupt_reg=-1, corrupt_bit, read_error=-1, write_error=-1;
static unsigned writes;
static int write_reg(const audio_codec_ctrl_if_t *c, int r, int n, const void *v, int len) {
    (void)c; assert(n==1 && len==1); writes++;
    if(r==write_error) return -1;
    regs[r]=r==0x0e ? reg0e : *(const uint8_t *)v; return 0;
}
static int read_reg(const audio_codec_ctrl_if_t *c, int r, int n, void *v, int len) {
    (void)c; assert(n==1 && len==1);
    *(uint8_t *)v=regs[r] ^ (r==corrupt_reg ? corrupt_bit : 0);
    return r==read_error ? -1 : 0;
}
'''
CASES = r'''
int main(void) {
    audio_codec_ctrl_if_t ctrl={write_reg, read_reg}; es_ctx_t c={&ctrl};
    // Actual device readback is 0x7f after the prescribed 0xff write.
    assert(es_force_suspend_once(&c)); assert(writes==15);
    assert(regs[0x0d]==0xfc && regs[0x45]==1 && regs[1]==0);
    // Only 0x0e bits 6:2 are defined in the datasheet. All must match;
    // differences in the three undocumented bits cannot prove a failure.
    for(unsigned reserved=0; reserved<8; ++reserved) {
        reg0e=0x7c | (reserved&3) | ((reserved&4)<<5);
        assert(es_force_suspend_once(&c));
    }
    reg0e=0x7f; corrupt_reg=0x0e;
    for(unsigned bit=2; bit<=6; ++bit) { corrupt_bit=1<<bit; assert(!es_force_suspend_once(&c)); }
    corrupt_reg=1; corrupt_bit=1; assert(!es_force_suspend_once(&c));
    corrupt_reg=-1; read_error=0x0e; assert(!es_force_suspend_once(&c));
    read_error=-1; write_error=0x0e; assert(!es_force_suspend_once(&c));
    return 0;
}
'''

class AudioSuspendTest(unittest.TestCase):
    def test_suspend_sequence_and_defined_readback_bits(self):
        source = (ROOT / 'components/platform/platform_esp32/src/audio_es8311.c').read_text()
        tables = source[source.index('typedef struct {\n    uint8_t reg;'):source.index('static int es_ctrl_write')]
        functions = '\n'.join(function(source, name) for name in
                              ('es_ctrl_write', 'es_ctrl_read', 'es_force_suspend_once'))
        with tempfile.TemporaryDirectory() as directory:
            src, exe = Path(directory)/'test.c', Path(directory)/'test'
            src.write_text(BOUNDARY + tables + functions + CASES)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(src),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,capture_output=True)
