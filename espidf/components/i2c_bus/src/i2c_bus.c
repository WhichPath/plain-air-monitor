#include "i2c_bus.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include <string.h>

static const char *TAG = "i2c_bus";

static SemaphoreHandle_t bus_mutex;
static i2c_master_bus_handle_t bus_handle;
static i2c_master_dev_handle_t device_handles[128];
static bool bus_ready;

static int timeout_to_ms(TickType_t ticks) {
    if (ticks == portMAX_DELAY) {
        return -1;
    }
    return (int)pdTICKS_TO_MS(ticks);
}

static esp_err_t get_device_locked(uint8_t address,
                                   i2c_master_dev_handle_t *out) {
    if (!out || address >= sizeof(device_handles) / sizeof(device_handles[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device_handles[address]) {
        *out = device_handles[address];
        return ESP_OK;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = SENSOR_I2C_FREQ_HZ,
        .scl_wait_us = 0,
    };

    esp_err_t err = i2c_master_bus_add_device(bus_handle, &dev_config,
                                              &device_handles[address]);
    if (err == ESP_OK) {
        *out = device_handles[address];
    }
    return err;
}

esp_err_t i2c_bus_init(void) {
    if (bus_ready) {
        return ESP_OK;
    }
    if (!bus_mutex) {
        bus_mutex = xSemaphoreCreateMutex();
        if (!bus_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    i2c_master_bus_config_t config = {
        .i2c_port = SENSOR_I2C_NUM,
        .sda_io_num = SENSOR_I2C_SDA_GPIO,
        .scl_io_num = SENSOR_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t err = i2c_new_master_bus(&config, &bus_handle);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    bus_ready = true;
    ESP_LOGI(TAG, "I2C%d ready SDA=%d SCL=%d freq=%d",
             SENSOR_I2C_NUM,
             SENSOR_I2C_SDA_GPIO,
             SENSOR_I2C_SCL_GPIO,
             SENSOR_I2C_FREQ_HZ);
    return ESP_OK;
}

bool i2c_bus_lock(TickType_t timeout_ticks) {
    return bus_mutex && xSemaphoreTake(bus_mutex, timeout_ticks) == pdTRUE;
}

void i2c_bus_unlock(void) {
    if (bus_mutex) {
        xSemaphoreGive(bus_mutex);
    }
}

esp_err_t i2c_bus_read(uint8_t address, uint8_t *data, size_t len,
                       TickType_t timeout_ticks) {
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!i2c_bus_lock(timeout_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_dev_handle_t dev = NULL;
    err = get_device_locked(address, &dev);
    if (err == ESP_OK) {
        err = i2c_master_receive(dev, data, len, timeout_to_ms(timeout_ticks));
    }
    i2c_bus_unlock();
    return err;
}

esp_err_t i2c_bus_write(uint8_t address, const uint8_t *data, size_t len,
                        TickType_t timeout_ticks) {
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!i2c_bus_lock(timeout_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_dev_handle_t dev = NULL;
    err = get_device_locked(address, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit(dev, data, len, timeout_to_ms(timeout_ticks));
    }
    i2c_bus_unlock();
    return err;
}

esp_err_t i2c_bus_probe(uint8_t address, TickType_t timeout_ticks) {
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!i2c_bus_lock(timeout_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    err = i2c_master_probe(bus_handle, address, timeout_to_ms(timeout_ticks));
    i2c_bus_unlock();
    return err;
}

esp_err_t i2c_bus_read_reg(uint8_t address, uint8_t reg, uint8_t *data,
                           size_t len, TickType_t timeout_ticks) {
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    if (!data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!i2c_bus_lock(timeout_ticks)) {
        return ESP_ERR_TIMEOUT;
    }
    i2c_master_dev_handle_t dev = NULL;
    err = get_device_locked(address, &dev);
    if (err == ESP_OK) {
        err = i2c_master_transmit_receive(dev, &reg, 1, data, len,
                                          timeout_to_ms(timeout_ticks));
    }
    i2c_bus_unlock();
    return err;
}

esp_err_t i2c_bus_write_reg(uint8_t address, uint8_t reg, const uint8_t *data,
                            size_t len, TickType_t timeout_ticks) {
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }
    if (len > 31) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t buffer[32];
    buffer[0] = reg;
    if (data && len > 0) {
        memcpy(&buffer[1], data, len);
    }
    return i2c_bus_write(address, buffer, len + 1, timeout_ticks);
}

void i2c_bus_sleep_us(uint32_t useconds) {
    if (useconds >= 1000) {
        vTaskDelay(pdMS_TO_TICKS((useconds + 999) / 1000));
    } else {
        esp_rom_delay_us(useconds);
    }
}

int16_t sensirion_i2c_hal_select_bus(uint8_t bus_idx) {
    return bus_idx == 0 ? NO_ERROR : NOT_IMPLEMENTED_ERROR;
}

void sensirion_i2c_hal_init(void) {
    (void)i2c_bus_init();
}

void sensirion_i2c_hal_free(void) {
}

int8_t sensirion_i2c_hal_read(uint8_t address, uint8_t *data, uint8_t count) {
    esp_err_t err = i2c_bus_read(address, data, count, pdMS_TO_TICKS(100));
    return err == ESP_OK ? NO_ERROR : I2C_BUS_ERROR;
}

int8_t sensirion_i2c_hal_write(uint8_t address, const uint8_t *data,
                               uint8_t count) {
    esp_err_t err = i2c_bus_write(address, data, count, pdMS_TO_TICKS(100));
    return err == ESP_OK ? NO_ERROR : I2C_BUS_ERROR;
}

void sensirion_i2c_hal_sleep_usec(uint32_t useconds) {
    i2c_bus_sleep_us(useconds);
}
