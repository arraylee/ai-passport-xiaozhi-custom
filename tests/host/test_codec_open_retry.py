"""Fault-inject the real SDK esp_codec_dev open/close beneath production es_rate.

Use the project's managed esp_codec_dev component (or ESP_CODEC_DEV_PATH).
Only codec registers, I2S hardware, logging, and unused software volume are
boundary doubles. SDK input/output-open flags and retry decisions are real.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_ble_advertising import function
from test_codec_i2s_failure import patcher

ROOT = Path(__file__).resolve().parents[2]
BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_codec_dev.h"
#include "audio_codec_ctrl_if.h"
#include "hal/hal_audio.h"
#include "esp_log.h"
typedef bool *i2s_chan_handle_t;
static unsigned channel_enables, channel_disables, formats, enables, closes;
static unsigned transfers, mute_calls;
static bool tx_running, rx_running, codec_enabled, hardware_muted;
static int fail_formats, fail_enables;
static int i2s_channel_enable(i2s_chan_handle_t channel) {
    assert(!*channel); *channel = true; channel_enables++; return 0;
}
static int i2s_channel_disable(i2s_chan_handle_t channel) {
    // IDF rejects an already-disabled channel without changing its state.
    // set_fmt may legitimately encounter this after a failed open rollback.
    if (!*channel) return -1;
    *channel = false; channel_disables++; return 0;
}
static int codec_enable(const audio_codec_if_t *h, bool enabled) {
    (void)h;
    if (enabled) {
        enables++;
        hardware_muted = false; // ES8311 can unmute even before enable fails.
        if (fail_enables) { fail_enables--; return ESP_CODEC_DEV_DRV_ERR; }
    } else {
        closes++;
        // ES8311 short-circuits disable if enable never completed.
        if (codec_enabled) hardware_muted = true;
    }
    codec_enabled = enabled; return 0;
}
static int codec_format(const audio_codec_if_t *h, esp_codec_dev_sample_info_t *fs) {
    (void)h; assert(fs->sample_rate > 0); formats++;
    if (fail_formats) { fail_formats--; return ESP_CODEC_DEV_DRV_ERR; }
    return 0;
}
static int codec_mute(const audio_codec_if_t *h, bool muted) {
    (void)h; hardware_muted = muted; mute_calls++; return 0;
}
static int codec_volume(const audio_codec_if_t *h, float v) { (void)h; (void)v; return 0; }
static int codec_gain(const audio_codec_if_t *h, float gain) { (void)h; (void)gain; return 0; }
static int data_format(const audio_codec_data_if_t *h, esp_codec_dev_type_t type,
                       esp_codec_dev_sample_info_t *fs) {
    (void)h; (void)fs; assert(type == ESP_CODEC_DEV_TYPE_IN_OUT);
    // Real audio_codec_data_i2s set_fmt disables both before reconfiguration.
    i2s_channel_disable(&tx_running); i2s_channel_disable(&rx_running); return 0;
}
static int data_enable(const audio_codec_data_if_t *h, esp_codec_dev_type_t type, bool enabled) {
    (void)h; assert(type == ESP_CODEC_DEV_TYPE_IN_OUT);
    if (enabled) { i2s_channel_enable(&tx_running); i2s_channel_enable(&rx_running); }
    else { i2s_channel_disable(&tx_running); i2s_channel_disable(&rx_running); }
    return 0;
}
static int data_io(const audio_codec_data_if_t *h, uint8_t *data, int length) {
    (void)h; (void)data; assert(length > 0 && codec_enabled && tx_running && rx_running);
    transfers++; return 0;
}
const audio_codec_vol_if_t *audio_codec_new_sw_vol(void) { abort(); }
int audio_codec_delete_vol_if(const audio_codec_vol_if_t *v) { (void)v; abort(); }
'''

CASES = r'''
static void run_case(bool fail_format, bool capture_mute) {
    formats = enables = closes = transfers = channel_enables = channel_disables = mute_calls = 0;
    codec_enabled = hardware_muted = false;
    tx_running = rx_running = true; // Production factory starts idle clocks.
    fail_formats = fail_format ? 2 : 0; fail_enables = fail_format ? 0 : 2;
    audio_codec_if_t codec = {.set_fs = codec_format, .enable = codec_enable,
        .mute = codec_mute, .set_vol = codec_volume, .set_mic_gain = codec_gain};
    audio_codec_data_if_t data = {.set_fmt = data_format, .enable = data_enable,
        .read = data_io, .write = data_io};
    esp_codec_dev_cfg_t cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT, .codec_if = &codec, .data_if = &data};
    es_ctx_t c = {.dev = esp_codec_dev_new(&cfg), .tx = &tx_running, .rx = &rx_running};
    assert(c.dev); hal_audio_t audio = {.impl = &c};
    assert(es_out_mute(&audio, capture_mute) == 0);
    char pcm[16] = {0}; size_t transferred = 123;
    for (unsigned attempt = 1; attempt <= 2; ++attempt) {
        assert(es_rate(&audio, 8000, 16, 1) != 0);
        assert(!c.opened && !c.cur_hz && !c.cur_bits && !c.cur_ch);
        assert(!codec_enabled && !tx_running && !rx_running && hardware_muted);
        assert(c.i2s_needs_enable && c.out_muted == capture_mute);
        assert(formats == attempt && closes == attempt);
        // The SDK must reject reads/writes after cleanup, not falsely succeed.
        assert(es_read(&audio, pcm, sizeof pcm, &transferred) != 0 && transferred == 0);
        assert(es_write(&audio, pcm, sizeof pcm, &transferred) != 0 && transferred == 0);
        assert(transfers == 0);
    }
    assert(es_rate(&audio, 8000, 16, 1) == 0);
    assert(c.opened && codec_enabled && tx_running && rx_running && formats == 3);
    assert(c.cur_hz == 8000 && c.cur_bits == 16 && c.cur_ch == 1);
    assert(!c.i2s_needs_enable && hardware_muted == capture_mute);
    assert(es_read(&audio, pcm, sizeof pcm, &transferred) == 0 && transferred == sizeof pcm);
    assert(es_write(&audio, pcm, sizeof pcm, &transferred) == 0 && transferred == sizeof pcm);
    unsigned same_format = formats, same_enables = channel_enables, same_mutes = mute_calls;
    assert(es_rate(&audio, 8000, 16, 1) == 0);
    assert(formats == same_format && channel_enables == same_enables && mute_calls == same_mutes);
    // A later format-change failure must clear the old cached format as well.
    fail_enables = 1;
    assert(es_rate(&audio, 16000, 16, 1) != 0);
    assert(!c.opened && !c.cur_hz && !codec_enabled && !tx_running && !rx_running && hardware_muted);
    assert(es_rate(&audio, 16000, 16, 1) == 0);
    assert(c.cur_hz == 16000 && codec_enabled && hardware_muted == capture_mute);
    assert(es_out_mute(&audio, !capture_mute) == 0 && hardware_muted != capture_mute);
    assert(es_rate(&audio, 8000, 16, 2) == 0 && hardware_muted != capture_mute);
    assert(c.cur_hz == 8000 && c.cur_ch == 2);
    es_close(&c);
    assert(!codec_enabled && !tx_running && !rx_running);
    assert(channel_disables == channel_enables + 2); // Includes factory's two initial enables.
    esp_codec_dev_delete(c.dev);
}
int main(void) {
    run_case(false, false); run_case(false, true);
    run_case(true, false); run_case(true, true);
    return 0;
}
'''


class CodecOpenRetryTest(unittest.TestCase):
    def test_partial_open_cleanup_retry_format_and_mute(self):
        codec = Path(os.environ.get('ESP_CODEC_DEV_PATH',
                                    ROOT / 'managed_components/espressif__esp_codec_dev'))
        self.assertTrue((codec / 'esp_codec_dev.c').is_file(),
                        'Install project SDK dependencies or set ESP_CODEC_DEV_PATH')
        idf = os.environ.get('IDF_PATH')
        description = ROOT / 'build/project_description.json'
        if not idf and description.is_file():
            idf = json.loads(description.read_text())['idf_path']
        self.assertTrue(idf, 'Set IDF_PATH or build once to locate the real SDK headers')
        source = (ROOT / 'components/platform/platform_esp32/src/audio_es8311.c').read_text()
        context = source[source.index('typedef struct {'):source.index('} es_ctx_t;') + len('} es_ctx_t;')]
        production = '\n'.join(function(source, name) for name in
                               ('es_meter_enable', 'es_write', 'es_read', 'es_out_mute', 'es_close', 'es_rate'))
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            (directory / 'esp_log.h').write_text(
                '#pragma once\n#define ESP_LOGI(...) ((void)0)\n'
                '#define ESP_LOGD(...) ((void)0)\n#define ESP_LOGE(...) ((void)0)\n')
            (directory / 'sdkconfig.h').write_text('#pragma once\n')
            src, exe = directory / 'test.c', directory / 'test'
            patched = directory / 'esp_codec_dev_fixed.c'
            patched.write_text(patcher.patch_device((codec / 'esp_codec_dev.c').read_text()))
            src.write_text(BOUNDARY + context + production + CASES)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            '-I' + str(directory), '-I' + str(codec), '-I' + str(codec / 'include'),
                            '-I' + str(codec / 'interface'), '-I' + str(codec / 'private_include'),
                            '-I' + str(Path(idf) / 'components/esp_common/include'),
                            '-I' + str(ROOT / 'components/core/ports/include'),
                            str(src), str(patched), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, capture_output=True)
