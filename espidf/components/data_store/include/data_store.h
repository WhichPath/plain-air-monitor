#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "sensor_service.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOURLY_RECORD_CAPACITY 168
#define HOURLY_WINDOW_MS (60LL * 60LL * 1000LL)

typedef struct {
    uint32_t seq;
    int64_t start_ms;
    int64_t end_ms;
    uint32_t sample_count;
    float pm1_0;
    float pm2_5;
    float pm4_0;
    float pm10_0;
} hourly_record_t;

typedef struct {
    int64_t start_ms;
    uint32_t sample_count;
    float pm1_0;
    float pm2_5;
    float pm4_0;
    float pm10_0;
} hourly_active_t;

esp_err_t data_store_init(void);
void data_store_add_sample(const sensor_sample_t *sample, void *user_data);

bool data_store_lock(TickType_t timeout_ticks);
void data_store_unlock(void);
uint16_t data_store_hourly_count_locked(void);
void data_store_get_active_locked(hourly_active_t *out);
bool data_store_get_hourly_record_locked(uint16_t order, hourly_record_t *out);
void data_store_get_summary(uint16_t *stored, uint32_t *active_samples, float *active_pm25);

#ifdef __cplusplus
}
#endif
