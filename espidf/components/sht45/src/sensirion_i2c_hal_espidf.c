#include "pm_sht45.h"

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"

static const char *TAG = "sht45_i2c";
static bool i2c_ready;

int16_t sensirion_i2c_select_bus(uint8_t bus_idx) {
    return bus_idx == 0 ? NO_ERROR : STATUS_FAIL;
}

void sensirion_i2c_init(void) {
    if (i2c_ready) {
        return;
    }

    i2c_config_t config = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PM_SHT45_I2C_SDA_GPIO,
        .scl_io_num = PM_SHT45_I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = PM_SHT45_I2C_FREQ_HZ,
        .clk_flags = 0,
    };

    esp_err_t err = i2c_param_config(PM_SHT45_I2C_NUM, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return;
    }

    err = i2c_driver_install(PM_SHT45_I2C_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_ready = true;
}

void sensirion_i2c_release(void) {
    if (i2c_ready) {
        i2c_driver_delete(PM_SHT45_I2C_NUM);
        i2c_ready = false;
    }
}

int8_t sensirion_i2c_read(uint8_t address, uint8_t *data, uint16_t count) {
    if (!i2c_ready) {
        sensirion_i2c_init();
    }
    esp_err_t err = i2c_master_read_from_device(PM_SHT45_I2C_NUM,
                                                address,
                                                data,
                                                count,
                                                pdMS_TO_TICKS(100));
    return err == ESP_OK ? NO_ERROR : STATUS_FAIL;
}

int8_t sensirion_i2c_write(uint8_t address, const uint8_t *data,
                           uint16_t count) {
    if (!i2c_ready) {
        sensirion_i2c_init();
    }
    esp_err_t err = i2c_master_write_to_device(PM_SHT45_I2C_NUM,
                                               address,
                                               data,
                                               count,
                                               pdMS_TO_TICKS(100));
    return err == ESP_OK ? NO_ERROR : STATUS_FAIL;
}

void sensirion_sleep_usec(uint32_t useconds) {
    if (useconds >= 1000) {
        vTaskDelay(pdMS_TO_TICKS((useconds + 999) / 1000));
    } else {
        esp_rom_delay_us(useconds);
    }
}
