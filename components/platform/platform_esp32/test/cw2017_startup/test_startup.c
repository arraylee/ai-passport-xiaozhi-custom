// Include the production implementation, not a reimplementation of its state machine.
#include "battery_cw2017.c"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint8_t registers[256];
static unsigned writes[256], reads[256], transactions, flag_sets, restarts, normal_writes;
static int fail_read = -1, fail_write = -1, corrupt_after_write = -1;
static int fail_read_after_write = -1, ignore_sleep_writes;
static int64_t now_us = 40LL * 24 * 60 * 60 * 1000000, normal_at;
static unsigned delays;
static bool nested_read, check_shutdown_reads;
static hal_battery_t *battery;

void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(unsigned ticks) {
    ++delays;
    now_us += ticks * 1000LL;
    if (check_shutdown_reads) {
        unsigned before = transactions;
        assert(hal_battery_read_soc(battery) == -1);
        assert(hal_battery_read_mv(battery) == -1);
        assert(transactions == before);
    }
}
int i2c_master_get_bus_handle(int port, i2c_master_bus_handle_t *bus) {
    (void)port; *bus = (void *)1; return ESP_OK;
}
int i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *bus) {
    (void)cfg; *bus = (void *)1; return ESP_OK;
}
int i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                            i2c_master_dev_handle_t *dev) {
    (void)bus; assert(cfg->device_address == 0x63); *dev = (void *)1; return ESP_OK;
}
int i2c_master_bus_rm_device(i2c_master_dev_handle_t dev) { (void)dev; return ESP_OK; }
int i2c_master_transmit_receive(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t tx_len,
                                uint8_t *rx, size_t rx_len, int timeout) {
    (void)dev; (void)timeout; assert(tx_len == 1);
    ++transactions;
    if (nested_read) {
        nested_read = false;
        unsigned before = transactions;
        assert(hal_battery_read_soc(battery) == -1);
        assert(hal_battery_read_mv(battery) == -1);
        assert(transactions == before);
    }
    for (size_t i = 0; i < rx_len; ++i) {
        unsigned reg = tx[0] + i;
        assert(reg < 256); ++reads[reg];
        if ((int)reg == fail_read || ((int)reg == fail_read_after_write && writes[reg])) return -1;
        rx[i] = registers[reg];
        if ((int)reg == corrupt_after_write && writes[reg]) rx[i] ^= 1;
    }
    return ESP_OK;
}
int i2c_master_transmit(i2c_master_dev_handle_t dev, const uint8_t *tx, size_t length, int timeout) {
    (void)dev; (void)timeout; assert(length == 2);
    unsigned reg = tx[0]; uint8_t value = tx[1];
    ++transactions; ++writes[reg];
    if ((int)reg == fail_write) return -1;
    if (reg == CW_REG_CONFIG) {
        if (value == CW_CONFIG_RESTART) ++restarts;
        if (value == CW_CONFIG_NORMAL) { ++normal_writes; normal_at = now_us; }
        if (value == CW_CONFIG_SLEEP && ignore_sleep_writes > 0) {
            --ignore_sleep_writes;
            return ESP_OK;
        }
    }
    if (reg == CW_REG_SOC_ALERT && (value & 0x80)) ++flag_sets;
    registers[reg] = value;
    return ESP_OK;
}

static hal_battery_t *create(void) {
    board_config_t cfg = BOARD_CONFIG_DEFAULT();
    return platform_create_battery(&cfg);
}
static void assert_no_writes(void) {
    for (unsigned i = 0; i < 256; ++i) assert(writes[i] == 0);
}
static void assert_immediate_soc(void) {
    unsigned before_delays = delays, before_writes = writes[CW_REG_CONFIG];
    assert(hal_battery_read_soc(battery) == 0); // A genuine empty battery stays empty.
    registers[CW_REG_SOC_H] = 100;
    assert(hal_battery_read_soc(battery) == 100);
    registers[CW_REG_SOC_H] = 101;
    assert(hal_battery_read_soc(battery) == -1);
    assert(delays == before_delays && writes[CW_REG_CONFIG] == before_writes);
}

int main(int argc, char **argv) {
    assert(argc == 3);
    const char *scenario = argv[1]; int argument = atoi(argv[2]);
    registers[CW_REG_VERSION] = 0xA0;
    registers[CW_REG_SOC_ALERT] = 0x85;
    registers[CW_REG_VCELL_H] = 0x2F; registers[CW_REG_VCELL_H + 1] = 0x80; // 3800 mV
    memcpy(registers + CW_REG_BATINFO, CW2017_PROFILES[CW2017_PROFILE_SEL].data, CW2017_PROFILE_LEN);

    if (!strcmp(scenario, "config_read_error")) {
        fail_read = CW_REG_CONFIG;
        assert(create() == NULL); assert_no_writes();
    } else if (!strcmp(scenario, "profile_read_error")) {
        fail_read = argument & 0xFF; registers[CW_REG_CONFIG] = (argument >> 8) & 0xFF;
        if (argument & (1 << 16)) registers[CW_REG_BATINFO] ^= 1;
        assert(create() == NULL); assert_no_writes();
    } else if (!strcmp(scenario, "wake_write_error") || !strcmp(scenario, "wake_verify_error")) {
        registers[CW_REG_CONFIG] = CW_CONFIG_SLEEP;
        if (!strcmp(scenario, "wake_write_error")) fail_write = CW_REG_CONFIG;
        else corrupt_after_write = CW_REG_CONFIG;
        assert(create() == NULL && writes[CW_REG_CONFIG] == 1 && normal_writes == 0);
        assert(reads[CW_REG_SOC_H] == 0);
    } else if (!strncmp(scenario, "profile_", 8) || !strcmp(scenario, "flag_write_error")) {
        registers[CW_REG_SOC_ALERT] &= ~0x80;
        registers[CW_REG_BATINFO] ^= 1;
        if (!strcmp(scenario, "profile_update") && argument) {
            registers[CW_REG_SOC_ALERT] |= 0x80;
            if (argument == 2) registers[CW_REG_CONFIG] = CW_CONFIG_SLEEP;
            if (argument == 3) registers[CW_REG_CONFIG] = CW_CONFIG_RESTART;
        }
        if (!strcmp(scenario, "profile_verify_error")) corrupt_after_write = CW_REG_BATINFO + argument;
        if (!strcmp(scenario, "profile_write_error")) fail_write = CW_REG_BATINFO + 79;
        if (!strcmp(scenario, "profile_verify_read_error")) fail_read_after_write = CW_REG_BATINFO + 79;
        if (!strcmp(scenario, "flag_write_error")) fail_write = CW_REG_SOC_ALERT;
        battery = create();
        if (strcmp(scenario, "profile_update")) {
            assert(battery == NULL); assert(flag_sets == 0);
        } else {
            assert(battery != NULL && flag_sets == 1 && restarts == 1 && normal_writes == 1);
            for (int i = 0; i < CW2017_PROFILE_LEN; ++i) {
                assert(writes[CW_REG_BATINFO + i] == 1);
                assert(registers[CW_REG_BATINFO + i] == CW2017_PROFILES[CW2017_PROFILE_SEL].data[i]);
            }
            assert((registers[CW_REG_SOC_ALERT] & 0x7F) == 5);
            assert_immediate_soc();
        }
    } else {
        bool wake = !strcmp(scenario, "wake") || !strcmp(scenario, "immediate") || !strcmp(scenario, "readiness");
        if (wake) registers[CW_REG_CONFIG] = argument ? argument : CW_CONFIG_SLEEP;
        battery = create(); assert(battery != NULL);
        assert(restarts == (wake ? 1u : 0u)); assert(normal_writes == (wake ? 1u : 0u));
        if (!wake) assert_no_writes();
        assert(now_us < 40LL * 24 * 60 * 60 * 1000000 + 200000); // No SOC polling delay.
        if (!strcmp(scenario, "immediate") || !strcmp(scenario, "wake")) {
            assert_immediate_soc();
        } else if (!strcmp(scenario, "readiness")) {
            registers[CW_REG_CONFIG] = CW_CONFIG_SLEEP;
            assert(hal_battery_read_soc(battery) == -1);
            registers[CW_REG_CONFIG] = CW_CONFIG_NORMAL;
            registers[CW_REG_VCELL_H] = registers[CW_REG_VCELL_H + 1] = 0;
            assert(hal_battery_read_soc(battery) == -1);
            registers[CW_REG_VCELL_H] = 0x2F;
            registers[CW_REG_SOC_H] = 255;
            assert(hal_battery_read_soc(battery) == -1);
            registers[CW_REG_SOC_H] = 0;
            fail_read = CW_REG_SOC_H;
            assert(hal_battery_read_soc(battery) == -1);
            fail_read = -1;
            assert(hal_battery_read_soc(battery) == 0);
            assert(restarts == 1 && normal_writes == 1);
        } else if (!strncmp(scenario, "shutdown", 8)) {
            if (!strcmp(scenario, "shutdown_retry")) ignore_sleep_writes = 1;
            if (!strcmp(scenario, "shutdown_failure")) ignore_sleep_writes = 2;
            check_shutdown_reads = true;
            unsigned config_reads = reads[CW_REG_CONFIG];
            bool failure = !strcmp(scenario, "shutdown_failure");
            assert(hal_battery_shutdown(battery) == (failure ? -1 : 0));
            unsigned attempts = !strcmp(scenario, "shutdown") ? 1 : 2;
            assert(writes[CW_REG_CONFIG] == attempts && reads[CW_REG_CONFIG] == config_reads + attempts);
            assert(failure || registers[CW_REG_CONFIG] == CW_CONFIG_SLEEP);
            unsigned before = transactions;
            assert(hal_battery_read_soc(battery) == -1 && hal_battery_read_mv(battery) == -1);
            assert(transactions == before);
        } else {
            if (!strcmp(scenario, "read_lock")) nested_read = true;
            assert(hal_battery_read_soc(battery) == 0);
            assert(hal_battery_read_mv(battery) == 3800);
            assert_no_writes();
        }
    }
    return 0;
}
