// AI Passport protocol 4 installer entry for the XiaoZhi USB image.
// A blank application boots the factory-installed permanent Recovery image.
// Powering on with UP held enters Recovery after a short debounce.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "bootloader_common.h"
#include "bootloader_config.h"
#include "bootloader_utility.h"
#include "esp_err.h"
#include "esp_image_format.h"
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
#define INSTALL_JOURNAL       0x6B8000
#define INSTALL_MARKER        0x49505234u

static bool has_image_header(size_t offset)
{
    uint32_t header;
    return bootloader_flash_read(offset, &header, sizeof(header), true) == ESP_OK &&
           (header & 0xff) == ESP_IMAGE_HEADER_MAGIC;
}

static bool install_in_progress(void)
{
    uint32_t word;
    if (bootloader_flash_read(INSTALL_JOURNAL, &word, sizeof(word), true) != ESP_OK) {
        return false;
    }
    // NOR starts erased. A power cut while writing the marker may leave some
    // of its zero bits at one. A .17 portrait at this address reads 0x7958c886.
    return word != UINT32_MAX && (word & INSTALL_MARKER) == INSTALL_MARKER;
}

static void try_permanent_recovery(const char *reason)
{
    bootloader_state_t state = { 0 };
    state.factory.offset = RECOVERY_OFFSET;
    state.factory.size = RECOVERY_SIZE;
    esp_image_metadata_t metadata;
    // The SDK loader resets if its only candidate is invalid. A USB install
    // can leave arbitrary old resources at this address, even an 0xe9 byte.
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &state.factory, &metadata) != ESP_OK) {
        ESP_LOGW("recovery_boot", "Invalid Recovery image, continuing normal boot");
        return;
    }
    ESP_LOGI("recovery_boot", "%s", reason);
    bootloader_utility_load_boot_image(&state, FACTORY_INDEX);
}

void bootloader_hooks_include(void)
{
}

void bootloader_after_init(void)
{
    if (!has_image_header(RECOVERY_OFFSET)) {
        return;
    }
    if (install_in_progress()) {
        try_permanent_recovery("Interrupted install: booting permanent recovery");
        return;
    }
    if (!has_image_header(APPLICATION_OFFSET)) {
        try_permanent_recovery("No application: booting permanent recovery");
        return;
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
        try_permanent_recovery("UP held: booting permanent recovery");
    }
}
