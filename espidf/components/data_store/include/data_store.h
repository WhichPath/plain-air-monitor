#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sensor_service.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DATA_RECORD_WINDOW_MS (10LL * 60LL * 1000LL)

#define DATA_FIELD_PM25 (1u << 0)
#define DATA_FIELD_PM10 (1u << 1)
#define DATA_FIELD_TEMPERATURE (1u << 2)
#define DATA_FIELD_HUMIDITY (1u << 3)

typedef struct {
    uint32_t seq;
    int64_t start_ms;
    int64_t end_ms;
    uint32_t duration_ms;
    uint16_t sample_count;
    uint16_t field_mask;
    float pm2_5_avg;
    float pm2_5_min;
    float pm2_5_max;
    float pm10_0_avg;
    float pm10_0_min;
    float pm10_0_max;
    float temperature_avg;
    float temperature_min;
    float temperature_max;
    float humidity_avg;
    float humidity_min;
    float humidity_max;
} data_record_t;

typedef struct {
    int64_t start_ms;
    uint32_t elapsed_ms;
    uint16_t sample_count;
    uint16_t field_mask;
    float pm2_5_avg;
    float pm2_5_min;
    float pm2_5_max;
    float pm10_0_avg;
    float pm10_0_min;
    float pm10_0_max;
    float temperature_avg;
    float temperature_min;
    float temperature_max;
    float humidity_avg;
    float humidity_min;
    float humidity_max;
} data_active_t;

esp_err_t data_store_init(void);
void data_store_add_sample(const sensor_sample_t *sample, void *user_data);

bool data_store_lock(TickType_t timeout_ticks);
void data_store_unlock(void);
uint32_t data_store_record_count_locked(void);
uint32_t data_store_record_capacity_locked(void);
void data_store_get_active_locked(data_active_t *out);
bool data_store_get_record_locked(uint32_t order, data_record_t *out);
void data_store_get_summary(uint32_t *stored, uint32_t *active_samples, float *active_pm25);

#ifdef __cplusplus
}
#endif
