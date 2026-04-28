#include "pm_scd41.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "scd4x_i2c.h"
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"
#include <string.h>

static const char *TAG = "pm_scd41";

static pm_scd41_status_t s_status = {
    .state = PM_SCD41_STATE_UNINITIALIZED,
};
static uint32_t last_pressure_hpa;

static void set_error(int err) {
    s_status.state = PM_SCD41_STATE_ERROR;
    s_status.last_error = err;
    s_status.error_count++;
}

esp_err_t pm_scd41_init(void) {
    if (s_status.state == PM_SCD41_STATE_MEASURING) {
        return ESP_OK;
    }

    s_status.state = PM_SCD41_STATE_INITIALIZING;
    s_status.last_error = 0;
    s_status.detected = false;
    last_pressure_hpa = 0;

    sensirion_i2c_hal_init();
    scd4x_init(SCD41_I2C_ADDR_62);
    sensirion_i2c_hal_sleep_usec(30000);

    (void)scd4x_wake_up();
    (void)scd4x_stop_periodic_measurement();
    (void)scd4x_reinit();

    uint16_t serial[3] = {0};
    int16_t rc = scd4x_get_serial_number(serial, 3);
    if (rc != NO_ERROR) {
        ESP_LOGW(TAG, "SCD41 probe failed rc=%d", rc);
        set_error(rc);
        return ESP_FAIL;
    }

    rc = scd4x_start_periodic_measurement();
    if (rc != NO_ERROR) {
        ESP_LOGW(TAG, "start periodic measurement failed rc=%d", rc);
        set_error(rc);
        return ESP_FAIL;
    }

    memcpy(s_status.serial, serial, sizeof(s_status.serial));
    s_status.state = PM_SCD41_STATE_MEASURING;
    s_status.last_error = 0;
    s_status.detected = true;
    ESP_LOGI(TAG, "SCD41 detected: serial=0x%04x%04x%04x",
             serial[0], serial[1], serial[2]);
    return ESP_OK;
}

esp_err_t pm_scd41_read(float pressure_pa, bool has_pressure,
                        pm_scd41_sample_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.state != PM_SCD41_STATE_MEASURING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (has_pressure && pressure_pa > 0.0f) {
        uint32_t hpa = (uint32_t)((pressure_pa / 100.0f) + 0.5f);
        if (hpa != last_pressure_hpa) {
            int16_t rc =
                scd4x_set_ambient_pressure((uint32_t)(pressure_pa + 0.5f));
            if (rc == NO_ERROR) {
                last_pressure_hpa = hpa;
            }
        }
    }

    bool ready = false;
    int16_t rc = scd4x_get_data_ready_status(&ready);
    if (rc != NO_ERROR) {
        s_status.last_error = rc;
        s_status.error_count++;
        return ESP_FAIL;
    }
    if (!ready) {
        return ESP_ERR_NOT_FINISHED;
    }

    uint16_t co2 = 0;
    int32_t temperature_mc = 0;
    int32_t humidity_mrh = 0;
    rc = scd4x_read_measurement(&co2, &temperature_mc, &humidity_mrh);
    if (rc != NO_ERROR) {
        s_status.last_error = rc;
        s_status.error_count++;
        ESP_LOGW(TAG, "read failed rc=%d", rc);
        return ESP_FAIL;
    }

    out->co2_ppm = co2;
    out->internal_temperature_c = temperature_mc / 1000.0f;
    out->internal_humidity_percent = humidity_mrh / 1000.0f;
    out->timestamp_ms = esp_timer_get_time() / 1000;
    s_status.last_error = 0;
    s_status.read_count++;
    return ESP_OK;
}

void pm_scd41_get_status(pm_scd41_status_t *out) {
    if (out) {
        *out = s_status;
    }
}

const char *pm_scd41_state_name(pm_scd41_state_t state) {
    switch (state) {
        case PM_SCD41_STATE_UNINITIALIZED: return "uninitialized";
        case PM_SCD41_STATE_INITIALIZING: return "initializing";
        case PM_SCD41_STATE_MEASURING: return "measuring";
        case PM_SCD41_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
