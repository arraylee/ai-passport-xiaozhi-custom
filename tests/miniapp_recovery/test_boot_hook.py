"""Exercise the actual early boot hook on direct USB and Recovery flash layouts."""
import subprocess
import tempfile
import unittest
from pathlib import Path

HOOK = Path(__file__).resolve().parents[2] / 'bootloader_components/recovery_boot_hook'
SDK = r'''
#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_OK 0
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_IMAGE_VERIFY_SILENT 1
#define FACTORY_INDEX (-1)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef int esp_err_t;
typedef int esp_image_metadata_t;
typedef struct { uint32_t offset, size; } esp_partition_pos_t;
typedef struct { esp_partition_pos_t factory; } bootloader_state_t;
static int GPIO;
static uint32_t journal, recovery, app, ms;
static bool valid, up, fail_read;
static unsigned verifies, jumped;
int bootloader_flash_read(size_t at, void *dst, size_t size, bool decrypt) {
    assert(size == 4 && decrypt);
    if (fail_read) return -1;
    switch (at) {
        case 0x6b8000: *(uint32_t *)dst=journal; break;
        case 0x6c0000: *(uint32_t *)dst=recovery; break;
        case 0x10000: *(uint32_t *)dst=app; break;
        default: assert(0);
    }
    return ESP_OK;
}
static int esp_image_verify(int mode, esp_partition_pos_t *slot, esp_image_metadata_t *m) {
    assert(mode==ESP_IMAGE_VERIFY_SILENT && slot->offset==0x6c0000 && slot->size==0x140000 && m);
    verifies++; return valid ? ESP_OK : -1;
}
static void bootloader_utility_load_boot_image(bootloader_state_t *state,int index) {
    assert(index==FACTORY_INDEX && state->factory.offset==0x6c0000 && valid);
    jumped++;
}
static void esp_rom_gpio_pad_select_gpio(int pin) { assert(pin==0); }
static void gpio_ll_input_enable(int *gpio, int pin) { assert(gpio==&GPIO && pin==0); }
#define gpio_ll_pullup_dis gpio_ll_input_enable
#define gpio_ll_pulldown_dis gpio_ll_input_enable
static int gpio_ll_get_level(int *gpio,int pin) { gpio_ll_input_enable(gpio,pin);return !up; }
static uint32_t esp_log_early_timestamp(void) { return ms; }
static void esp_rom_delay_us(uint32_t us) { ms+=us/1000; assert(ms<2000); }
'''
CASES = r'''
static void run(uint32_t word, uint32_t rec, uint32_t main, bool held, bool good,
                unsigned expected_checks, unsigned expected_jumps) {
    ms=verifies=jumped=0; journal=word; recovery=rec; app=main; up=held; valid=good;
    bootloader_after_init();
    assert(verifies==expected_checks && jumped==expected_jumps);
}
int main(void) {
    const uint32_t magic=0x49505234, image=0xe9, erased=UINT32_MAX;
    /* Exact bytes measured on a .17 card after a USB .18 overlay. */
    run(0x7958c886,0xb1081590,image,false,false,0,0);
    run(0x7958c886,image,image,false,true,0,0);
    run(magic,erased,image,true,false,0,0);
    run(magic,image,image,false,false,1,0);
    run(erased,image,image,true,false,1,0);
    run(erased,image,image,false,true,0,0);
    run(erased,image,image,true,true,1,1);
    run(erased,image,erased,false,true,1,1);
    run(magic,image,image,false,true,1,1);
    for (unsigned bit=0;bit<32;bit++) {
        /* A torn NOR write can leave bits which should be zero still at one. */
        if (!(magic & (1u<<bit))) run(magic | (1u<<bit),image,image,false,true,1,1);
    }
    fail_read=true;run(magic,image,image,true,true,0,0);
}
'''


class RecoveryBoot(unittest.TestCase):
    def test_direct_usb_and_recovery_boots(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / 'fake.h').write_text(SDK)
            for name in ('bootloader_common.h','bootloader_config.h','bootloader_utility.h','esp_err.h',
                         'esp_image_format.h','esp_log.h','esp_rom_gpio.h','esp_rom_sys.h','hal/gpio_ll.h'):
                header = root / name
                header.parent.mkdir(exist_ok=True)
                header.write_text('#include "fake.h"\n')
            test = root / 'test.c'
            test.write_text('#include "recovery_boot_hook.c"\n' + CASES)
            exe = root / 'test'
            subprocess.run(['cc','-std=c11','-Werror','-I',str(root),'-I',str(HOOK),
                            str(test),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)


if __name__=='__main__':
    unittest.main()
