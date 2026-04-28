#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_HISTORY_CAPACITY 24
#define SENSOR_RAW_SAMPLE_INTERVAL_MS 1000
#define SENSOR_FRAME_INTERVAL_MS 5000

typedef enum {
    SENSOR_STATE_UNINITIALIZED = 0,
    SENSOR_STATE_INITIALIZING,
    SENSOR_STATE_CONDITIONING,
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
    float pressure_pa;
    uint16_t co2_ppm;
    float voc_index;
    float nox_index;
    bool has_temperature;
    bool has_humidity;
    bool has_pressure;
    bool has_co2;
    bool has_voc_index;
    bool has_nox_index;
    uint16_t pm_count;
    uint16_t temperature_count;
    uint16_t humidity_count;
    uint16_t pressure_count;
    uint16_t co2_count;
    uint16_t voc_index_count;
    uint16_t nox_index_count;
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
    sensor_state_t bmp581_state;
    int bmp581_last_error;
    uint32_t bmp581_error_count;
    uint32_t bmp581_read_count;
    uint8_t bmp581_address;
    uint8_t bmp581_chip_id;
    bool bmp581_detected;
    sensor_state_t scd41_state;
    int scd41_last_error;
    uint32_t scd41_error_count;
    uint32_t scd41_read_count;
    uint16_t scd41_serial[3];
    bool scd41_detected;
    sensor_state_t sgp41_state;
    int sgp41_last_error;
    uint32_t sgp41_error_count;
    uint32_t sgp41_read_count;
    uint16_t sgp41_serial[3];
    uint16_t sgp41_self_test_result;
    uint8_t sgp41_conditioning_remaining_s;
    bool sgp41_detected;
} sensor_status_t;

typedef struct {
    int64_t timestamp_ms;
    float pm1_0;
    float pm2_5;
    float pm10_0;
    float temperature_c;
    float humidity_percent;
    float pressure_pa;
    uint16_t co2_ppm;
    float voc_index;
    float nox_index;
    bool has_temperature;
    bool has_humidity;
    bool has_pressure;
    bool has_co2;
    bool has_voc_index;
    bool has_nox_index;
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
