"""Exercise the actual SDK volume interpolation and ES8311 register conversion."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_ble_advertising import function

ROOT = Path(__file__).resolve().parents[2]
SDK = ROOT / "managed_components/espressif__esp_codec_dev"

BOUNDARY = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include "esp_codec_dev_vol.h"
typedef void audio_codec_if_t;
typedef void *esp_codec_dev_handle_t;
typedef struct {bool is_open; float hw_gain;} audio_codec_es8311_t;
#define ES8311_DAC_REG32 0x32
#define ESP_LOGD(...)
static unsigned reg32;
static int curve_error;
static esp_codec_dev_vol_map_t copied[2];
static esp_codec_dev_vol_curve_t calibrated;
static int esp_codec_dev_set_vol_curve(void *dev,esp_codec_dev_vol_curve_t *curve) {
    assert(dev && curve->count==2);
    if(curve_error)return -5;
    copied[0]=curve->vol_map[0];copied[1]=curve->vol_map[1];
    calibrated=(esp_codec_dev_vol_curve_t){.count=2,.vol_map=copied};return 0;
}
static int es8311_write_reg(audio_codec_es8311_t*c,int r,uint8_t value) {
    assert(c && r==0x32);reg32=value;return 0;
}
'''

CASES = r'''
int main(void) {
    esp_codec_dev_hw_gain_t hardware={.pa_voltage=5.0f,.codec_dac_voltage=3.3f};
    audio_codec_es8311_t codec={.is_open=true,.hw_gain=esp_codec_dev_col_calc_hw_gain(&hardware)};
    esp_codec_dev_vol_curve_t old={0};assert(_get_default_vol_curve(&old)==0);
    assert(es8311_set_vol((void*)&codec,_get_vol_db(&old,95))==0);
    assert(reg32>191); //0dB is register191; the shipped SDK default boosts near-full-scale PCM.
    float before=esp_codec_dev_vol_calc_db(&vol_range,reg32);
    assert(powf(10.0f,before/20.0f)*32767.0f>32767.0f);
    assert(es_output_curve(&codec)==0);
    unsigned previous=0;
    for(unsigned percent=0;percent<=100;percent++) {
        assert(es8311_set_vol((void*)&codec,_get_vol_db(&calibrated,percent))==0);
        assert(reg32>=previous && reg32<=182);previous=reg32;
        float db=esp_codec_dev_vol_calc_db(&vol_range,reg32);
        assert(powf(10.0f,db/20.0f)*32767.0f<20000.0f);
    }
    float maximum=esp_codec_dev_vol_calc_db(&vol_range,reg32);
    assert(es8311_set_vol((void*)&codec,_get_vol_db(&calibrated,95))==0);
    float duplex=esp_codec_dev_vol_calc_db(&vol_range,reg32);
    assert(duplex<=-6.0f); // Retain the old measured80% echo-safe output ceiling.
    curve_error=1;assert(es_output_curve(&codec)==-5);
    free(old.vol_map);
    printf("Actual SDK + ES8311: old95%% = %.1fdB, new maximum = %.1fdB, duplex ceiling = %.1fdB; all101 levels monotonic and below full scale.\n",before,maximum,duplex);
}
'''


class OutputGainTest(unittest.TestCase):
    def test_dac_headroom_with_real_volume_math(self):
        hal = (ROOT / "components/platform/platform_esp32/src/audio_es8311.c").read_text()
        codec = (SDK / "device/es8311/es8311.c").read_text()
        api = (SDK / "esp_codec_dev.c").read_text()
        start = codec.index("static const esp_codec_dev_vol_range_t vol_range")
        table = codec[start:codec.index("};", start)+2]
        real = table + "\n" + "\n".join((function(api, "_get_default_vol_curve"),
                  function(api, "_get_vol_db"), function(codec, "es8311_set_vol"),
                  function(hal, "es_output_curve")))
        with tempfile.TemporaryDirectory() as directory:
            source, executable = Path(directory)/"gain.c", Path(directory)/"gain"
            (Path(directory)/"esp_err.h").write_text("#pragma once\n#define ESP_FAIL -1\n#define ESP_ERR_INVALID_ARG -2\n#define ESP_ERR_NO_MEM -3\n#define ESP_ERR_NOT_SUPPORTED -4\n#define ESP_ERR_NOT_FOUND -5\n#define ESP_ERR_INVALID_STATE -6\n")
            source.write_text(BOUNDARY + real + CASES)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                            "-I"+directory, "-I"+str(SDK/"include"), str(source), str(SDK/"esp_codec_dev_vol.c"),
                            "-lm", "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
