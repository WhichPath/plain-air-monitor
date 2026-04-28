#include "pm_sht45.h"

#include "i2c_bus.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sensirion_common.h"

static bool i2c_ready;

int16_t sensirion_i2c_select_bus(uint8_t bus_idx) {
    return bus_idx == 0 ? NO_ERROR : STATUS_FAIL;
}

void sensirion_i2c_init(void) {
    if (i2c_ready) {
        return;
    }

    i2c_ready = i2c_bus_init() == ESP_OK;
}

void sensirion_i2c_release(void) {
    i2c_ready = false;
}

int8_t sensirion_i2c_read(uint8_t address, uint8_t *data, uint16_t count) {
    if (!i2c_ready) {
        sensirion_i2c_init();
    }
    esp_err_t err = i2c_bus_read(address, data, count, pdMS_TO_TICKS(100));
    return err == ESP_OK ? NO_ERROR : STATUS_FAIL;
}

int8_t sensirion_i2c_write(uint8_t address, const uint8_t *data,
                           uint16_t count) {
    if (!i2c_ready) {
        sensirion_i2c_init();
    }
    esp_err_t err = i2c_bus_write(address, data, count, pdMS_TO_TICKS(100));
    return err == ESP_OK ? NO_ERROR : STATUS_FAIL;
}

void sensirion_sleep_usec(uint32_t useconds) {
    i2c_bus_sleep_us(useconds);
}
