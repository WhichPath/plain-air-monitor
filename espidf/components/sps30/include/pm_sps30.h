#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PM_SPS30_UART_NUM       1
#define PM_SPS30_UART_RX_GPIO   16
#define PM_SPS30_UART_TX_GPIO   17
#define PM_SPS30_UART_BAUD      115200

typedef enum {
    PM_SPS30_STATE_UNINITIALIZED = 0,
    PM_SPS30_STATE_INITIALIZING,
    PM_SPS30_STATE_MEASURING,
    PM_SPS30_STATE_ERROR,
} pm_sps30_state_t;

typedef struct {
    float pm1_0;
    float pm2_5;
    float pm4_0;
    float pm10_0;
    float nc0_5;
    float nc1_0;
    float nc2_5;
    float nc4_0;
    float nc10_0;
    float typical_particle_size;
    int64_t timestamp_ms;
} pm_sps30_sample_t;

typedef struct {
    pm_sps30_state_t state;
    int last_error;
    uint32_t error_count;
    uint32_t read_count;
} pm_sps30_status_t;

esp_err_t pm_sps30_init(void);
esp_err_t pm_sps30_read(pm_sps30_sample_t *out);
void pm_sps30_stop(void);
void pm_sps30_get_status(pm_sps30_status_t *out);
const char *pm_sps30_state_name(pm_sps30_state_t state);

#ifdef __cplusplus
}
#endif
