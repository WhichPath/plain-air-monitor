#include "pm_sgp41.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sensirion_common.h"
#include "sensirion_gas_index_algorithm.h"
#include "sensirion_i2c_hal.h"
#include "sgp41_i2c.h"
#include <string.h>

static const char *TAG = "pm_sgp41";

#define SGP41_DEFAULT_RH_TICKS 0x8000
#define SGP41_DEFAULT_T_TICKS  0x6666
#define SGP41_CONDITIONING_S   10

static pm_sgp41_status_t s_status = {
    .state = PM_SGP41_STATE_UNINITIALIZED,
};
static GasIndexAlgorithmParams voc_params;
static GasIndexAlgorithmParams nox_params;

static void set_error(int err) {
    s_status.state = PM_SGP41_STATE_ERROR;
    s_status.last_error = err;
    s_status.error_count++;
}

static uint16_t clamp_u16(float value) {
    if (value <= 0.0f) {
        return 0;
    }
    if (value >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)(value + 0.5f);
}

static void compensation_ticks(float temperature_c, float humidity_percent,
                               bool has_compensation, uint16_t *rh_ticks,
                               uint16_t *t_ticks) {
    if (!has_compensation) {
        *rh_ticks = SGP41_DEFAULT_RH_TICKS;
        *t_ticks = SGP41_DEFAULT_T_TICKS;
        return;
    }

    *rh_ticks = clamp_u16(humidity_percent * 65535.0f / 100.0f);
    *t_ticks = clamp_u16((temperature_c + 45.0f) * 65535.0f / 175.0f);
}

esp_err_t pm_sgp41_init(void) {
    if (s_status.state == PM_SGP41_STATE_MEASURING ||
        s_status.state == PM_SGP41_STATE_CONDITIONING) {
        return ESP_OK;
    }

    s_status.state = PM_SGP41_STATE_INITIALIZING;
    s_status.last_error = 0;
    s_status.detected = false;
    s_status.conditioning_remaining_s = SGP41_CONDITIONING_S;

    sensirion_i2c_hal_init();

    uint16_t serial[3] = {0};
    int16_t rc = sgp41_get_serial_number(serial);
    if (rc != NO_ERROR) {
        ESP_LOGW(TAG, "SGP41 probe failed rc=%d", rc);
        set_error(rc);
        return ESP_FAIL;
    }

    uint16_t test_result = 0;
    rc = sgp41_execute_self_test(&test_result);
    if (rc != NO_ERROR || (test_result & 0x000F) != 0) {
        ESP_LOGW(TAG, "SGP41 self-test failed rc=%d result=0x%04x", rc,
                 test_result);
        set_error(rc != NO_ERROR ? rc : test_result);
        return ESP_FAIL;
    }

    GasIndexAlgorithm_init(&voc_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    GasIndexAlgorithm_init(&nox_params, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);

    memcpy(s_status.serial, serial, sizeof(s_status.serial));
    s_status.self_test_result = test_result;
    s_status.state = PM_SGP41_STATE_CONDITIONING;
    s_status.detected = true;
    s_status.last_error = 0;
    ESP_LOGI(TAG, "SGP41 detected: serial=0x%04x%04x%04x",
             serial[0], serial[1], serial[2]);
    return ESP_OK;
}

esp_err_t pm_sgp41_read(float temperature_c, float humidity_percent,
                        bool has_compensation, pm_sgp41_sample_t *out) {
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_status.state != PM_SGP41_STATE_MEASURING &&
        s_status.state != PM_SGP41_STATE_CONDITIONING) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(out, 0, sizeof(*out));
    uint16_t rh_ticks = SGP41_DEFAULT_RH_TICKS;
    uint16_t t_ticks = SGP41_DEFAULT_T_TICKS;
    compensation_ticks(temperature_c, humidity_percent, has_compensation,
                       &rh_ticks, &t_ticks);

    int16_t rc = NO_ERROR;
    if (s_status.conditioning_remaining_s > 0) {
        rc = sgp41_execute_conditioning(rh_ticks, t_ticks, &out->sraw_voc);
        s_status.conditioning_remaining_s--;
        out->has_nox = false;
        if (s_status.conditioning_remaining_s == 0) {
            s_status.state = PM_SGP41_STATE_MEASURING;
        }
    } else {
        rc = sgp41_measure_raw_signals(rh_ticks, t_ticks, &out->sraw_voc,
                                       &out->sraw_nox);
        out->has_nox = true;
    }
    if (rc != NO_ERROR) {
        s_status.last_error = rc;
        s_status.error_count++;
        ESP_LOGW(TAG, "read failed rc=%d", rc);
        return ESP_FAIL;
    }

    GasIndexAlgorithm_process(&voc_params, out->sraw_voc, &out->voc_index);
    if (out->has_nox) {
        GasIndexAlgorithm_process(&nox_params, out->sraw_nox, &out->nox_index);
    }
    out->timestamp_ms = esp_timer_get_time() / 1000;
    s_status.last_error = 0;
    s_status.read_count++;
    return ESP_OK;
}

void pm_sgp41_get_status(pm_sgp41_status_t *out) {
    if (out) {
        *out = s_status;
    }
}

const char *pm_sgp41_state_name(pm_sgp41_state_t state) {
    switch (state) {
        case PM_SGP41_STATE_UNINITIALIZED: return "uninitialized";
        case PM_SGP41_STATE_INITIALIZING: return "initializing";
        case PM_SGP41_STATE_CONDITIONING: return "conditioning";
        case PM_SGP41_STATE_MEASURING: return "measuring";
        case PM_SGP41_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
