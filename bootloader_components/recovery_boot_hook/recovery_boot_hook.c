// Bootloader hook shared by every AI Passport community firmware derivative.
// A blank application boots the factory-installed permanent Recovery image.
// Powering on with UP held enters Recovery after a short debounce.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bootloader_common.h"
#include "bootloader_config.h"
#include "bootloader_utility.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "recovery_hold.h"

// ESP-IDF 5.5.3 declares this in bootloader_flash_priv.h, which pulls in
// application-only SPI flash headers unavailable in the bootloader build.
extern esp_err_t bootloader_flash_read(size_t src_addr, void *dest, size_t size,
                                        bool allow_decrypt);

#define RECOVERY_BUTTON_GPIO  0
#define RECOVERY_OFFSET       0x6C0000
#define RECOVERY_SIZE         0x140000
#define APPLICATION_OFFSET    0x10000
#define ESP_IMAGE_MAGIC       0xE9

static void boot_permanent_recovery(void)
{
    bootloader_state_t state = { 0 };
    state.factory.offset = RECOVERY_OFFSET;
    state.factory.size = RECOVERY_SIZE;
    bootloader_utility_load_boot_image(&state, FACTORY_INDEX);
}

void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    uint32_t pending = 0xffffffff;
    if (bootloader_flash_read(0x6b8000, &pending, sizeof(pending), true) == ESP_OK &&
        pending != 0xffffffff) {
        ESP_LOGI("recovery_boot", "Interrupted install: booting permanent recovery");
        boot_permanent_recovery();
    }
    uint32_t image_header = 0;
    if (bootloader_flash_read(APPLICATION_OFFSET, &image_header,
                              sizeof(image_header), true) == ESP_OK &&
        (image_header & 0xff) != ESP_IMAGE_MAGIC) {
        ESP_LOGI("recovery_boot", "No application: booting permanent recovery at 0x%x",
                 RECOVERY_OFFSET);
        boot_permanent_recovery();
    }

    // GPIO0 has an external pull-up in the board's resistor ladder.
    esp_rom_gpio_pad_select_gpio(RECOVERY_BUTTON_GPIO);
    gpio_ll_input_enable(&GPIO, RECOVERY_BUTTON_GPIO);
    gpio_ll_pullup_dis(&GPIO, RECOVERY_BUTTON_GPIO);
    gpio_ll_pulldown_dis(&GPIO, RECOVERY_BUTTON_GPIO);
    recovery_hold_t hold = { .started = esp_log_early_timestamp() };
    ESP_LOGI("recovery_boot", "Power on with UP held to enter Recovery");
    int result;
    do {
        result = recovery_hold_sample(&hold, esp_log_early_timestamp(),
                                      gpio_ll_get_level(&GPIO, RECOVERY_BUTTON_GPIO) == 0);
        esp_rom_delay_us(10000);
    } while (result == 0);
    if (result == 1) {
        ESP_LOGI("recovery_boot", "UP held: booting permanent recovery at 0x%x",
                 RECOVERY_OFFSET);
        boot_permanent_recovery();
    }
}
