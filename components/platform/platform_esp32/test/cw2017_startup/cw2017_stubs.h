#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "platform/board_config.h"
#include "hal/hal_battery.h"

typedef void *i2c_master_dev_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef struct {
    int i2c_port, sda_io_num, scl_io_num, clk_source, glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags;
} i2c_master_bus_config_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
#define ESP_OK 0
#define I2C_NUM_0 0
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) test_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) test_log(tag, __VA_ARGS__)

void test_log(const char *tag, const char *format, ...);
int64_t esp_timer_get_time(void);
void vTaskDelay(unsigned ticks);
int i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t,
                                uint8_t *, size_t, int);
int i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
int i2c_master_get_bus_handle(int, i2c_master_bus_handle_t *);
int i2c_new_master_bus(const i2c_master_bus_config_t *, i2c_master_bus_handle_t *);
int i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *,
                            i2c_master_dev_handle_t *);
int i2c_master_bus_rm_device(i2c_master_dev_handle_t);
