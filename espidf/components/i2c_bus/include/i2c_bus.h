#pragma once

#include "driver/i2c_types.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_I2C_NUM      I2C_NUM_0
#define SENSOR_I2C_SDA_GPIO 43
#define SENSOR_I2C_SCL_GPIO 44
#define SENSOR_I2C_FREQ_HZ  100000

esp_err_t i2c_bus_init(void);
bool i2c_bus_lock(TickType_t timeout_ticks);
void i2c_bus_unlock(void);

esp_err_t i2c_bus_read(uint8_t address, uint8_t *data, size_t len,
                       TickType_t timeout_ticks);
esp_err_t i2c_bus_write(uint8_t address, const uint8_t *data, size_t len,
                        TickType_t timeout_ticks);
esp_err_t i2c_bus_probe(uint8_t address, TickType_t timeout_ticks);
esp_err_t i2c_bus_read_reg(uint8_t address, uint8_t reg, uint8_t *data,
                           size_t len, TickType_t timeout_ticks);
esp_err_t i2c_bus_write_reg(uint8_t address, uint8_t reg, const uint8_t *data,
                            size_t len, TickType_t timeout_ticks);
void i2c_bus_sleep_us(uint32_t useconds);

#ifdef __cplusplus
}
#endif
