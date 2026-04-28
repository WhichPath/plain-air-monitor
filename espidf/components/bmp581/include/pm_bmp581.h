#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_BMP581_I2C_ADDR_PRIMARY   0x46
#define PM_BMP581_I2C_ADDR_SECONDARY 0x47

typedef enum {
    PM_BMP581_STATE_UNINITIALIZED = 0,
    PM_BMP581_STATE_INITIALIZING,
    PM_BMP581_STATE_MEASURING,
    PM_BMP581_STATE_ERROR,
} pm_bmp581_state_t;

typedef struct {
    float pressure_pa;
    float temperature_c;
    int64_t timestamp_ms;
} pm_bmp581_sample_t;

typedef struct {
    pm_bmp581_state_t state;
    int last_error;
    uint32_t error_count;
    uint32_t read_count;
    uint8_t address;
    uint8_t chip_id;
    bool detected;
} pm_bmp581_status_t;

esp_err_t pm_bmp581_init(void);
esp_err_t pm_bmp581_read(pm_bmp581_sample_t *out);
void pm_bmp581_get_status(pm_bmp581_status_t *out);
const char *pm_bmp581_state_name(pm_bmp581_state_t state);

#ifdef __cplusplus
}
#endif
