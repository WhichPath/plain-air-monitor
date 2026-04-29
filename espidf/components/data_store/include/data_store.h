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

#define DATA_FIELD_PM25        (1u << 0)
#define DATA_FIELD_PM10        (1u << 1)
#define DATA_FIELD_TEMPERATURE (1u << 2)
#define DATA_FIELD_HUMIDITY    (1u << 3)
#define DATA_FIELD_PRESSURE    (1u << 4)
#define DATA_FIELD_CO2         (1u << 5)
#define DATA_FIELD_VOC_INDEX   (1u << 6)
#define DATA_FIELD_NOX_INDEX   (1u << 7)

typedef struct {
    float avg;
    float min;
    float max;
    uint16_t count;
} data_field_stat_t;

typedef struct {
    uint32_t seq;
    int64_t end_epoch_ms;
    int64_t end_uptime_ms;
    uint16_t frame_count;
    uint16_t field_mask;
    bool time_verified;
    bool time_reconciled;
    data_field_stat_t pm2_5;
    data_field_stat_t pm10_0;
    data_field_stat_t temperature;
    data_field_stat_t humidity;
    data_field_stat_t pressure;
    data_field_stat_t co2;
    data_field_stat_t voc_index;
    data_field_stat_t nox_index;
} data_record_t;

typedef struct {
    int64_t end_epoch_ms;
    int64_t end_uptime_ms;
    uint16_t frame_count;
    uint16_t field_mask;
    bool time_verified;
    bool time_reconciled;
    data_field_stat_t pm2_5;
    data_field_stat_t pm10_0;
    data_field_stat_t temperature;
    data_field_stat_t humidity;
    data_field_stat_t pressure;
    data_field_stat_t co2;
    data_field_stat_t voc_index;
    data_field_stat_t nox_index;
} data_active_t;

esp_err_t data_store_init(void);
void data_store_add_sample(const sensor_sample_t *sample, void *user_data);

bool data_store_lock(TickType_t timeout_ticks);
void data_store_unlock(void);
uint32_t data_store_record_count_locked(void);
uint32_t data_store_record_capacity_locked(void);
void data_store_get_active_locked(data_active_t *out);
bool data_store_get_record_locked(uint32_t order, data_record_t *out);
void data_store_get_summary(uint32_t *stored, uint32_t *active_frames,
                            float *active_pm25);

#ifdef __cplusplus
}
#endif
