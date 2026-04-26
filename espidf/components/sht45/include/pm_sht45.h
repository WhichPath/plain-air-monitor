#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_SHT45_I2C_NUM       0
#define PM_SHT45_I2C_SDA_GPIO  43
#define PM_SHT45_I2C_SCL_GPIO  44
#define PM_SHT45_I2C_FREQ_HZ   100000
#define PM_SHT45_I2C_ADDR      0x44

typedef enum {
    PM_SHT45_STATE_UNINITIALIZED = 0,
    PM_SHT45_STATE_INITIALIZING,
    PM_SHT45_STATE_MEASURING,
    PM_SHT45_STATE_ERROR,
} pm_sht45_state_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
    int64_t timestamp_ms;
} pm_sht45_sample_t;

typedef struct {
    pm_sht45_state_t state;
    int last_error;
    uint32_t error_count;
    uint32_t read_count;
    uint32_t serial;
    bool detected;
} pm_sht45_status_t;

esp_err_t pm_sht45_init(void);
esp_err_t pm_sht45_read(pm_sht45_sample_t *out);
void pm_sht45_get_status(pm_sht45_status_t *out);
const char *pm_sht45_state_name(pm_sht45_state_t state);

#ifdef __cplusplus
}
#endif
