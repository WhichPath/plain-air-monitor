#include "pm_sht45.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sensirion_i2c.h"
#include "sht4x.h"
#include <string.h>

static const char *TAG = "pm_sht45";

static pm_sht45_status_t s_status = {
    .state = PM_SHT45_STATE_UNINITIALIZED,
};

static void set_error(int err) {
    s_status.state = PM_SHT45_STATE_ERROR;
    s_status.last_error = err;
    s_status.error_count++;
}

esp_err_t pm_sht45_init(void) {
    if (s_status.state == PM_SHT45_STATE_MEASURING) {
        return ESP_OK;
    }

    s_status.state = PM_SHT45_STATE_INITIALIZING;
    s_status.last_error = 0;
    s_status.detected = false;

    sensirion_i2c_init();

    uint32_t serial = 0;
    int16_t rc = sht4x_read_serial(&serial);
    if (rc != STATUS_OK) {
        ESP_LOGW(TAG, "SHT45 probe failed on I2C%d SDA=%d SCL=%d addr=0x%02x rc=%d",
                 PM_SHT45_I2C_NUM,
                 PM_SHT45_I2C_SDA_GPIO,
                 PM_SHT45_I2C_SCL_GPIO,
                 PM_SHT45_I2C_ADDR,
                 rc);
        set_error(rc);
        return ESP_FAIL;
    }

    s_status.state = PM_SHT45_STATE_MEASURING;
    s_status.last_error = 0;
    s_status.detected = true;
    s_status.serial = serial;
    ESP_LOGI(TAG, "SHT45 detected: serial=0x%08lx I2C%d SDA=%d SCL=%d addr=0x%02x",
             (unsigned long)serial,
             PM_SHT45_I2C_NUM,
             PM_SHT45_I2C_SDA_GPIO,
             PM_SHT45_I2C_SCL_GPIO,
             PM_SHT45_I2C_ADDR);
    return ESP_OK;
}

esp_err_t pm_sht45_read(pm_sht45_sample_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.state != PM_SHT45_STATE_MEASURING) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));

    int32_t temperature_milli_c = 0;
    int32_t humidity_milli_percent = 0;
    int16_t rc = sht4x_measure_blocking_read(&temperature_milli_c,
                                             &humidity_milli_percent);
    if (rc != STATUS_OK) {
        s_status.last_error = rc;
        s_status.error_count++;
        ESP_LOGW(TAG, "read failed: %d", rc);
        return ESP_FAIL;
    }

    out->temperature_c = temperature_milli_c / 1000.0f;
    out->humidity_percent = humidity_milli_percent / 1000.0f;
    out->timestamp_ms = esp_timer_get_time() / 1000;
    s_status.last_error = 0;
    s_status.read_count++;
    return ESP_OK;
}

void pm_sht45_get_status(pm_sht45_status_t *out) {
    if (out) {
        *out = s_status;
    }
}

const char *pm_sht45_state_name(pm_sht45_state_t state) {
    switch (state) {
        case PM_SHT45_STATE_UNINITIALIZED: return "uninitialized";
        case PM_SHT45_STATE_INITIALIZING: return "initializing";
        case PM_SHT45_STATE_MEASURING: return "measuring";
        case PM_SHT45_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
