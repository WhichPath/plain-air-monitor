#include "pm_sps30.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensirion_shdlc.h"
#include "sensirion_uart_hal.h"
#include "sps30_uart.h"
#include <string.h>

static const char *TAG = "pm_sps30";

static pm_sps30_status_t s_status = {
    .state = PM_SPS30_STATE_UNINITIALIZED,
};

static void set_error(int err) {
    s_status.state = PM_SPS30_STATE_ERROR;
    s_status.last_error = err;
    s_status.error_count++;
}

esp_err_t pm_sps30_init(void) {
    s_status.state = PM_SPS30_STATE_INITIALIZING;
    s_status.last_error = 0;

    int16_t rc = sensirion_uart_hal_init(NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "UART HAL init failed: %d", rc);
        set_error(rc);
        return ESP_FAIL;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        rc = sps30_wake_up_sequence();
        if (rc == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "wake-up failed: %d", rc);
        set_error(rc);
        return ESP_FAIL;
    }

    rc = sps30_start_measurement(SPS30_OUTPUT_FORMAT_OUTPUT_FORMAT_FLOAT);
    if (rc != 0) {
        ESP_LOGE(TAG, "start measurement failed: %d", rc);
        set_error(rc);
        return ESP_FAIL;
    }

    s_status.state = PM_SPS30_STATE_MEASURING;
    s_status.last_error = 0;
    ESP_LOGI(TAG, "SPS30 measuring on UART%d RX=%d TX=%d",
             PM_SPS30_UART_NUM, PM_SPS30_UART_RX_GPIO, PM_SPS30_UART_TX_GPIO);
    return ESP_OK;
}

esp_err_t pm_sps30_read(pm_sps30_sample_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.state != PM_SPS30_STATE_MEASURING) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    int16_t rc = sps30_read_measurement_values_float(
        &out->pm1_0, &out->pm2_5, &out->pm4_0, &out->pm10_0,
        &out->nc0_5, &out->nc1_0, &out->nc2_5, &out->nc4_0,
        &out->nc10_0, &out->typical_particle_size);

    if (rc != 0) {
        s_status.last_error = rc;
        s_status.error_count++;
        ESP_LOGW(TAG, "read failed: %d", rc);
        return ESP_FAIL;
    }

    out->timestamp_ms = esp_timer_get_time() / 1000;
    s_status.last_error = 0;
    s_status.read_count++;
    return ESP_OK;
}

void pm_sps30_stop(void) {
    if (s_status.state == PM_SPS30_STATE_MEASURING) {
        int16_t rc = sps30_stop_measurement();
        if (rc != 0) {
            ESP_LOGW(TAG, "stop measurement failed: %d", rc);
        }
    }
    sensirion_uart_hal_free();
    s_status.state = PM_SPS30_STATE_UNINITIALIZED;
}

void pm_sps30_get_status(pm_sps30_status_t *out) {
    if (out) {
        *out = s_status;
    }
}

const char *pm_sps30_state_name(pm_sps30_state_t state) {
    switch (state) {
        case PM_SPS30_STATE_UNINITIALIZED: return "uninitialized";
        case PM_SPS30_STATE_INITIALIZING: return "initializing";
        case PM_SPS30_STATE_MEASURING: return "measuring";
        case PM_SPS30_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
