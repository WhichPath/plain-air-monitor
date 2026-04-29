#include "pm_bmp581.h"

#include "bmp5.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "i2c_bus.h"
#include <string.h>

static const char *TAG = "pm_bmp581";

static pm_bmp581_status_t s_status = {
    .state = PM_BMP581_STATE_UNINITIALIZED,
};
static struct bmp5_dev s_dev;
static struct bmp5_osr_odr_press_config s_cfg;
static uint8_t s_addr;

static void set_error(int err) {
    s_status.state = PM_BMP581_STATE_ERROR;
    s_status.last_error = err;
    s_status.error_count++;
}

static BMP5_INTF_RET_TYPE bmp_i2c_read(uint8_t reg_addr, uint8_t *reg_data,
                                       uint32_t len, void *intf_ptr) {
    uint8_t address = *(uint8_t *)intf_ptr;
    esp_err_t err = i2c_bus_read_reg(address, reg_addr, reg_data, len,
                                     pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C read failed addr=0x%02x reg=0x%02x len=%lu: %s",
                 address, reg_addr, (unsigned long)len, esp_err_to_name(err));
        return BMP5_E_COM_FAIL;
    }
    return BMP5_INTF_RET_SUCCESS;
}

static BMP5_INTF_RET_TYPE bmp_i2c_write(uint8_t reg_addr,
                                        const uint8_t *reg_data,
                                        uint32_t len, void *intf_ptr) {
    uint8_t address = *(uint8_t *)intf_ptr;
    esp_err_t err = i2c_bus_write_reg(address, reg_addr, reg_data, len,
                                      pdMS_TO_TICKS(100));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C write failed addr=0x%02x reg=0x%02x len=%lu: %s",
                 address, reg_addr, (unsigned long)len, esp_err_to_name(err));
        return BMP5_E_COM_FAIL;
    }
    return BMP5_INTF_RET_SUCCESS;
}

static void bmp_delay_us(uint32_t period, void *intf_ptr) {
    (void)intf_ptr;
    i2c_bus_sleep_us(period);
}

static void init_dev(uint8_t address) {
    memset(&s_dev, 0, sizeof(s_dev));
    s_addr = address;
    s_dev.intf = BMP5_I2C_INTF;
    s_dev.intf_ptr = &s_addr;
    s_dev.read = bmp_i2c_read;
    s_dev.write = bmp_i2c_write;
    s_dev.delay_us = bmp_delay_us;
}

static bool chip_id_valid(uint8_t chip_id) {
    return chip_id == BMP5_CHIP_ID_PRIM || chip_id == BMP5_CHIP_ID_SEC;
}

static int8_t configure_sensor(void) {
    int8_t rslt = bmp5_set_power_mode(BMP5_POWERMODE_STANDBY, &s_dev);
    if (rslt != BMP5_OK) {
        return rslt;
    }

    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.press_en = BMP5_ENABLE;
    s_cfg.osr_t = BMP5_OVERSAMPLING_4X;
    s_cfg.osr_p = BMP5_OVERSAMPLING_16X;
    s_cfg.odr = BMP5_ODR_01_HZ;
    rslt = bmp5_set_osr_odr_press_config(&s_cfg, &s_dev);
    if (rslt != BMP5_OK) {
        return rslt;
    }

    struct bmp5_iir_config iir = {
        .set_iir_t = BMP5_IIR_FILTER_COEFF_1,
        .set_iir_p = BMP5_IIR_FILTER_COEFF_1,
        .shdw_set_iir_t = BMP5_ENABLE,
        .shdw_set_iir_p = BMP5_ENABLE,
        .iir_flush_forced_en = BMP5_ENABLE,
    };
    return bmp5_set_iir_config(&iir, &s_dev);
}

esp_err_t pm_bmp581_init(void) {
    if (s_status.state == PM_BMP581_STATE_MEASURING) {
        return ESP_OK;
    }

    s_status.state = PM_BMP581_STATE_INITIALIZING;
    s_status.last_error = 0;
    s_status.detected = false;

    const uint8_t addresses[] = {
        PM_BMP581_I2C_ADDR_PRIMARY,
        PM_BMP581_I2C_ADDR_SECONDARY,
    };
    int8_t rslt = BMP5_E_DEV_NOT_FOUND;
    for (size_t i = 0; i < sizeof(addresses); i++) {
        uint8_t chip_id = 0;
        bool has_valid_chip_id = false;
        esp_err_t probe_err = i2c_bus_probe(addresses[i], pdMS_TO_TICKS(100));
        if (probe_err != ESP_OK) {
            ESP_LOGW(TAG, "BMP581 address 0x%02x did not ACK: %s",
                     addresses[i], esp_err_to_name(probe_err));
        } else {
            esp_err_t read_err = i2c_bus_read_reg(addresses[i],
                                                  BMP5_REG_CHIP_ID, &chip_id,
                                                  1, pdMS_TO_TICKS(100));
            if (read_err == ESP_OK) {
                ESP_LOGI(TAG, "BMP581 address 0x%02x chip-id register=0x%02x",
                         addresses[i], chip_id);
                has_valid_chip_id = chip_id_valid(chip_id);
            } else {
                ESP_LOGW(TAG, "BMP581 address 0x%02x chip-id read failed: %s",
                         addresses[i], esp_err_to_name(read_err));
            }
        }
        init_dev(addresses[i]);
        rslt = bmp5_init(&s_dev);
        if (rslt == BMP5_E_POWER_UP && has_valid_chip_id) {
            uint8_t nvm_status = 0;
            int8_t status_rslt = bmp5_get_regs(BMP5_REG_STATUS, &nvm_status,
                                               1, &s_dev);
            if (status_rslt == BMP5_OK &&
                (nvm_status & BMP5_INT_NVM_RDY) &&
                !(nvm_status & BMP5_INT_NVM_ERR)) {
                s_dev.chip_id = chip_id;
                rslt = BMP5_OK;
                ESP_LOGW(TAG,
                         "BMP581 addr=0x%02x accepted after cleared POR status; nvm_status=0x%02x",
                         addresses[i], nvm_status);
            }
        }
        if (rslt == BMP5_OK) {
            break;
        }
        ESP_LOGW(TAG, "BMP581 init failed at addr=0x%02x rc=%d",
                 addresses[i], rslt);
    }
    if (rslt != BMP5_OK) {
        ESP_LOGW(TAG, "BMP581 probe failed rc=%d", rslt);
        set_error(rslt);
        return ESP_FAIL;
    }

    rslt = configure_sensor();
    if (rslt != BMP5_OK) {
        ESP_LOGW(TAG, "BMP581 config failed rc=%d", rslt);
        set_error(rslt);
        return ESP_FAIL;
    }

    s_status.state = PM_BMP581_STATE_MEASURING;
    s_status.last_error = 0;
    s_status.detected = true;
    s_status.address = s_addr;
    s_status.chip_id = s_dev.chip_id;
    ESP_LOGI(TAG, "BMP581 detected: addr=0x%02x chip=0x%02x", s_addr,
             s_dev.chip_id);
    return ESP_OK;
}

esp_err_t pm_bmp581_read(pm_bmp581_sample_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.state != PM_BMP581_STATE_MEASURING) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    int8_t rslt = bmp5_set_power_mode(BMP5_POWERMODE_FORCED, &s_dev);
    if (rslt != BMP5_OK) {
        set_error(rslt);
        return ESP_FAIL;
    }
    i2c_bus_sleep_us(50000);

    struct bmp5_sensor_data data;
    rslt = bmp5_get_sensor_data(&data, &s_cfg, &s_dev);
    if (rslt != BMP5_OK) {
        s_status.last_error = rslt;
        s_status.error_count++;
        ESP_LOGW(TAG, "read failed: %d", rslt);
        return ESP_FAIL;
    }

    out->pressure_pa = data.pressure;
    out->temperature_c = data.temperature;
    out->timestamp_ms = esp_timer_get_time() / 1000;
    s_status.last_error = 0;
    s_status.read_count++;
    return ESP_OK;
}

void pm_bmp581_get_status(pm_bmp581_status_t *out) {
    if (out) {
        *out = s_status;
    }
}

const char *pm_bmp581_state_name(pm_bmp581_state_t state) {
    switch (state) {
        case PM_BMP581_STATE_UNINITIALIZED: return "uninitialized";
        case PM_BMP581_STATE_INITIALIZING: return "initializing";
        case PM_BMP581_STATE_MEASURING: return "measuring";
        case PM_BMP581_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
