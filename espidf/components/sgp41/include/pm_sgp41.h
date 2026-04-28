#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PM_SGP41_STATE_UNINITIALIZED = 0,
    PM_SGP41_STATE_INITIALIZING,
    PM_SGP41_STATE_CONDITIONING,
    PM_SGP41_STATE_MEASURING,
    PM_SGP41_STATE_ERROR,
} pm_sgp41_state_t;

typedef struct {
    uint16_t sraw_voc;
    uint16_t sraw_nox;
    int32_t voc_index;
    int32_t nox_index;
    bool has_nox;
    int64_t timestamp_ms;
} pm_sgp41_sample_t;

typedef struct {
    pm_sgp41_state_t state;
    int last_error;
    uint32_t error_count;
    uint32_t read_count;
    uint16_t serial[3];
    uint16_t self_test_result;
    uint8_t conditioning_remaining_s;
    bool detected;
} pm_sgp41_status_t;

esp_err_t pm_sgp41_init(void);
esp_err_t pm_sgp41_read(float temperature_c, float humidity_percent,
                        bool has_compensation, pm_sgp41_sample_t *out);
void pm_sgp41_get_status(pm_sgp41_status_t *out);
const char *pm_sgp41_state_name(pm_sgp41_state_t state);

#ifdef __cplusplus
}
#endif
