#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_HISTORY_CAPACITY 1800
#define SENSOR_SAMPLE_INTERVAL_MS 2000

typedef enum {
    SENSOR_STATE_UNINITIALIZED = 0,
    SENSOR_STATE_INITIALIZING,
    SENSOR_STATE_MEASURING,
    SENSOR_STATE_ERROR,
} sensor_state_t;

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
    float temperature_c;
    float humidity_percent;
    bool has_temperature;
    bool has_humidity;
    int64_t timestamp_ms;
} sensor_sample_t;

typedef struct {
    sensor_state_t state;
    int last_error;
    uint32_t error_count;
    uint32_t read_count;
    uint32_t app_read_failures;
    sensor_state_t sht45_state;
    int sht45_last_error;
    uint32_t sht45_error_count;
    uint32_t sht45_read_count;
    uint32_t sht45_serial;
    bool sht45_detected;
} sensor_status_t;

typedef struct {
    int64_t timestamp_ms;
    float pm1_0;
    float pm2_5;
    float pm10_0;
    float temperature_c;
    float humidity_percent;
    bool has_temperature;
    bool has_humidity;
} sensor_history_point_t;

typedef void (*sensor_sample_callback_t)(const sensor_sample_t *sample, void *user_data);

esp_err_t sensor_service_start(sensor_sample_callback_t on_sample, void *user_data);
bool sensor_service_get_latest(sensor_sample_t *out);
void sensor_service_get_status(sensor_status_t *out);
uint16_t sensor_service_get_history_count(void);
bool sensor_service_get_history_point(uint16_t order, sensor_history_point_t *out);
const char *sensor_state_name(sensor_state_t state);

#ifdef __cplusplus
}
#endif
