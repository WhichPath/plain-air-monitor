#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_SCD41_I2C_ADDR 0x62

typedef enum {
    PM_SCD41_STATE_UNINITIALIZED = 0,
    PM_SCD41_STATE_INITIALIZING,
    PM_SCD41_STATE_MEASURING,
    PM_SCD41_STATE_ERROR,
} pm_scd41_state_t;

typedef struct {
    uint16_t co2_ppm;
    float internal_temperature_c;
    float internal_humidity_percent;
    int64_t timestamp_ms;
} pm_scd41_sample_t;

typedef struct {
    pm_scd41_state_t state;
    int last_error;
    uint32_t error_count;
    uint32_t read_count;
    uint16_t serial[3];
    bool detected;
} pm_scd41_status_t;

esp_err_t pm_scd41_init(void);
esp_err_t pm_scd41_read(float pressure_pa, bool has_pressure,
                        pm_scd41_sample_t *out);
void pm_scd41_get_status(pm_scd41_status_t *out);
const char *pm_scd41_state_name(pm_scd41_state_t state);

#ifdef __cplusplus
}
#endif
