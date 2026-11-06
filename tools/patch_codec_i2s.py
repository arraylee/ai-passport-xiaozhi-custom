"""Generate the pinned esp_codec_dev I2S failure-handling correction."""
from pathlib import Path
import argparse
import hashlib

UPSTREAM_SHA256 = 'b30d62d4e3e4a2dbfcdb95c6b137109f88997a19281236c6f8fe16a8f52dc9d7'
DEVICE_SHA256 = 'b7a17e2ad412f4c08e0e9324aea0f217a49305510fd80e7ef3420fb31d7fd2ce'

def patch_device(source):
    if hashlib.sha256(source.encode()).hexdigest() != DEVICE_SHA256:
        raise ValueError('esp_codec_dev 1.5.10 device source changed; review before upgrading')
    start = source.index('int esp_codec_dev_open(')
    end = source.index('\nint esp_codec_dev_read_reg(', start)
    body = source[start:end]
    changes = [
        ('    if (data_if->set_fmt) {\n        data_if->set_fmt(data_if, dev->dev_caps, fs);\n    }',
         '    int ret = ESP_CODEC_DEV_OK;\n    if (data_if->set_fmt) {\n        ret = data_if->set_fmt(data_if, dev->dev_caps, fs);\n        if (ret != ESP_CODEC_DEV_OK) goto open_failed;\n    }'),
        ('        data_if->enable(data_if, dev->dev_caps, true);',
         '        ret = data_if->enable(data_if, dev->dev_caps, true);\n        if (ret != ESP_CODEC_DEV_OK) goto open_failed;'),
        ('            if (codec->set_fs(codec, fs) != 0) {\n                return ESP_CODEC_DEV_NOT_SUPPORT;\n            }',
         '            ret = codec->set_fs(codec, fs);\n            if (ret != ESP_CODEC_DEV_OK) goto open_failed;'),
        ('            if (codec->enable(codec, true) != ESP_CODEC_DEV_OK) {\n                ESP_LOGE(TAG, "Fail to enable codec");\n                return ESP_CODEC_DEV_DRV_ERR;\n            }',
         '            ret = codec->enable(codec, true);\n            if (ret != ESP_CODEC_DEV_OK) goto open_failed;'),
        ('    return ESP_CODEC_DEV_OK;\n}', '''    return ESP_CODEC_DEV_OK;
open_failed:
    /* A format failure can leave DMA descriptors absent. Never enable them;
     * close partial codec/data state so the next open really reconfigures. */
    if (codec && codec->enable) (void)codec->enable(codec, false);
    if (data_if->enable) (void)data_if->enable(data_if, dev->dev_caps, false);
    dev->input_opened = dev->output_opened = false;
    return ret;
}'''),
    ]
    for before, after in changes:
        if body.count(before) != 1:
            raise ValueError('Expected exactly one codec open patch site')
        body = body.replace(before, after)
    return source[:start] + body + source[end:]

def patch(source):
    if hashlib.sha256(source.encode()).hexdigest() != UPSTREAM_SHA256:
        raise ValueError('esp_codec_dev 1.5.10 I2S source changed; review the patch before upgrading')
    changes = [
        ('''        ret = _i2s_drv_enable(i2s_data, true, enable);
        ret = _i2s_drv_enable(i2s_data, false, enable);''', '''        ret = _i2s_drv_enable(i2s_data, true, enable);
        if (!enable) {
            int rx_ret = _i2s_drv_enable(i2s_data, false, false);
            if (ret == ESP_CODEC_DEV_OK) ret = rx_ret;
        } else if (ret == ESP_CODEC_DEV_OK) {
            ret = _i2s_drv_enable(i2s_data, false, true);
            if (ret != ESP_CODEC_DEV_OK) (void)_i2s_drv_enable(i2s_data, true, false);
        }
        if (ret != ESP_CODEC_DEV_OK) enable = false;'''),
        ('''        ret = set_fs(i2s_data, true, true);
        ret = set_fs(i2s_data, false, true);''', '''        ret = set_fs(i2s_data, true, true);
        /* A failed reconfiguration frees DMA descriptors. Preserve that error;
         * a later successful RX configuration must not authorize TX startup. */
        if (ret == ESP_CODEC_DEV_OK) {
            ret = set_fs(i2s_data, false, true);
        }'''),
        ('''        _i2s_drv_enable(paired, true, false);
        ret = set_drv_fs(channel, true, bits_per_sample, i2s_data->clk_src, fs);
        _i2s_drv_enable(paired, true, true);''', '''        ret = _i2s_drv_enable(paired, true, false);
        if (ret != ESP_CODEC_DEV_OK) return ret;
        ret = set_drv_fs(channel, true, bits_per_sample, i2s_data->clk_src, fs);
        if (ret == ESP_CODEC_DEV_OK) {
            ret = _i2s_drv_enable(paired, true, true);
        }'''),
    ]
    for before, after in changes:
        if source.count(before) != 1:
            raise ValueError('Expected exactly one I2S patch site')
        source = source.replace(before, after)
    return source

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--device-input', type=Path, required=True)
    parser.add_argument('--device-output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(patch(args.input.read_text()))
    args.device_output.write_text(patch_device(args.device_input.read_text()))
