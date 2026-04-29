#include "sensor_service.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pm_bmp581.h"
#include "pm_scd41.h"
#include "pm_sgp41.h"
#include "pm_sht45.h"
#include "pm_sps30.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sensor_service";

#define SENSOR_RETRY_INTERVAL_TICKS 5
#define SPS30_REINIT_FAILURES 10
#define I2C_SENSOR_REINIT_FAILURES 3
#define FRAME_TICKS (SENSOR_FRAME_INTERVAL_MS / SENSOR_RAW_SAMPLE_INTERVAL_MS)

typedef struct {
    double sum;
    uint16_t count;
} frame_scalar_t;

typedef struct {
    frame_scalar_t pm1_0;
    frame_scalar_t pm2_5;
    frame_scalar_t pm4_0;
    frame_scalar_t pm10_0;
    frame_scalar_t nc0_5;
    frame_scalar_t nc1_0;
    frame_scalar_t nc2_5;
    frame_scalar_t nc4_0;
    frame_scalar_t nc10_0;
    frame_scalar_t typical_particle_size;
    frame_scalar_t temperature_c;
    frame_scalar_t humidity_percent;
    frame_scalar_t pressure_pa;
    frame_scalar_t co2_ppm;
    frame_scalar_t voc_index;
    frame_scalar_t nox_index;
} frame_accumulator_t;

static SemaphoreHandle_t sensor_mutex;
static sensor_sample_t latest_sample;
static sensor_sample_t *history;
static uint16_t history_head;
static uint16_t history_count;
static bool latest_valid;
static uint32_t read_failures;
static TaskHandle_t task_handle;
static sensor_sample_callback_t sample_callback;
static void *sample_callback_data;

static bool sps30_ready;
static bool sht45_ready;
static bool bmp581_ready;
static bool scd41_ready;
static bool sgp41_ready;
static int sps30_retry_countdown;
static int sht45_retry_countdown;
static int bmp581_retry_countdown;
static int scd41_retry_countdown;
static int sgp41_retry_countdown;
static int sps30_consecutive_failures;
static int sht45_consecutive_failures;
static int bmp581_consecutive_failures;
static int scd41_consecutive_failures;
static int sgp41_consecutive_failures;
static float latest_temperature_c;
static float latest_humidity_percent;
static bool has_latest_sht45;
static float latest_pressure_pa;
static bool has_latest_pressure;
static frame_accumulator_t frame_acc;
static uint16_t frame_tick_count;

static sensor_state_t map_sht45_state(pm_sht45_state_t state) {
    switch (state) {
        case PM_SHT45_STATE_INITIALIZING: return SENSOR_STATE_INITIALIZING;
        case PM_SHT45_STATE_MEASURING: return SENSOR_STATE_MEASURING;
        case PM_SHT45_STATE_ERROR: return SENSOR_STATE_ERROR;
        case PM_SHT45_STATE_UNINITIALIZED:
        default: return SENSOR_STATE_UNINITIALIZED;
    }
}

static sensor_state_t map_sps30_state(pm_sps30_state_t state) {
    switch (state) {
        case PM_SPS30_STATE_INITIALIZING: return SENSOR_STATE_INITIALIZING;
        case PM_SPS30_STATE_MEASURING: return SENSOR_STATE_MEASURING;
        case PM_SPS30_STATE_ERROR: return SENSOR_STATE_ERROR;
        case PM_SPS30_STATE_UNINITIALIZED:
        default: return SENSOR_STATE_UNINITIALIZED;
    }
}

static sensor_state_t map_bmp581_state(pm_bmp581_state_t state) {
    switch (state) {
        case PM_BMP581_STATE_INITIALIZING: return SENSOR_STATE_INITIALIZING;
        case PM_BMP581_STATE_MEASURING: return SENSOR_STATE_MEASURING;
        case PM_BMP581_STATE_ERROR: return SENSOR_STATE_ERROR;
        case PM_BMP581_STATE_UNINITIALIZED:
        default: return SENSOR_STATE_UNINITIALIZED;
    }
}

static sensor_state_t map_scd41_state(pm_scd41_state_t state) {
    switch (state) {
        case PM_SCD41_STATE_INITIALIZING: return SENSOR_STATE_INITIALIZING;
        case PM_SCD41_STATE_MEASURING: return SENSOR_STATE_MEASURING;
        case PM_SCD41_STATE_ERROR: return SENSOR_STATE_ERROR;
        case PM_SCD41_STATE_UNINITIALIZED:
        default: return SENSOR_STATE_UNINITIALIZED;
    }
}

static sensor_state_t map_sgp41_state(pm_sgp41_state_t state) {
    switch (state) {
        case PM_SGP41_STATE_INITIALIZING: return SENSOR_STATE_INITIALIZING;
        case PM_SGP41_STATE_CONDITIONING: return SENSOR_STATE_CONDITIONING;
        case PM_SGP41_STATE_MEASURING: return SENSOR_STATE_MEASURING;
        case PM_SGP41_STATE_ERROR: return SENSOR_STATE_ERROR;
        case PM_SGP41_STATE_UNINITIALIZED:
        default: return SENSOR_STATE_UNINITIALIZED;
    }
}

static void scalar_add(frame_scalar_t *acc, float value) {
    acc->sum += value;
    acc->count++;
}

static float scalar_avg(const frame_scalar_t *acc) {
    return acc->count > 0 ? (float)(acc->sum / acc->count) : 0.0f;
}

static void retry_init(bool *ready, int *countdown, esp_err_t (*init_fn)(void),
                       const char *name) {
    if (*ready) {
        return;
    }
    if (*countdown > 0) {
        (*countdown)--;
        return;
    }
    *ready = (init_fn() == ESP_OK);
    if (!*ready) {
        *countdown = SENSOR_RETRY_INTERVAL_TICKS;
        ESP_LOGD(TAG, "%s not available; retrying later", name);
    }
}

static bool handle_read_failure(const char *name, bool *ready, int *countdown,
                                int *failures) {
    read_failures++;
    (*failures)++;
    if (*failures < I2C_SENSOR_REINIT_FAILURES) {
        ESP_LOGD(TAG, "%s read failed (%d/%d)", name, *failures,
                 I2C_SENSOR_REINIT_FAILURES);
        return false;
    }

    ESP_LOGW(TAG, "%s read failed %d times, reinitializing", name, *failures);
    *ready = false;
    *countdown = 0;
    *failures = 0;
    return true;
}

static void read_sps30(void) {
    retry_init(&sps30_ready, &sps30_retry_countdown, pm_sps30_init, "SPS30");
    if (!sps30_ready) {
        return;
    }

    pm_sps30_sample_t sample;
    esp_err_t err = pm_sps30_read(&sample);
    if (err != ESP_OK) {
        read_failures++;
        sps30_consecutive_failures++;
        if (sps30_consecutive_failures >= SPS30_REINIT_FAILURES) {
            ESP_LOGW(TAG, "too many SPS30 failures, reinitializing");
            pm_sps30_stop();
            sps30_ready = false;
            sps30_retry_countdown = 0;
            sps30_consecutive_failures = 0;
        }
        return;
    }

    sps30_consecutive_failures = 0;
    scalar_add(&frame_acc.pm1_0, sample.pm1_0);
    scalar_add(&frame_acc.pm2_5, sample.pm2_5);
    scalar_add(&frame_acc.pm4_0, sample.pm4_0);
    scalar_add(&frame_acc.pm10_0, sample.pm10_0);
    scalar_add(&frame_acc.nc0_5, sample.nc0_5);
    scalar_add(&frame_acc.nc1_0, sample.nc1_0);
    scalar_add(&frame_acc.nc2_5, sample.nc2_5);
    scalar_add(&frame_acc.nc4_0, sample.nc4_0);
    scalar_add(&frame_acc.nc10_0, sample.nc10_0);
    scalar_add(&frame_acc.typical_particle_size, sample.typical_particle_size);
}

static void read_sht45(void) {
    retry_init(&sht45_ready, &sht45_retry_countdown, pm_sht45_init, "SHT45");
    if (!sht45_ready) {
        return;
    }

    pm_sht45_sample_t sample;
    if (pm_sht45_read(&sample) != ESP_OK) {
        if (handle_read_failure("SHT45", &sht45_ready, &sht45_retry_countdown,
                                &sht45_consecutive_failures)) {
            has_latest_sht45 = false;
        }
        return;
    }

    sht45_consecutive_failures = 0;
    latest_temperature_c = sample.temperature_c;
    latest_humidity_percent = sample.humidity_percent;
    has_latest_sht45 = true;
    scalar_add(&frame_acc.temperature_c, sample.temperature_c);
    scalar_add(&frame_acc.humidity_percent, sample.humidity_percent);
}

static void read_sgp41(void) {
    retry_init(&sgp41_ready, &sgp41_retry_countdown, pm_sgp41_init, "SGP41");
    if (!sgp41_ready) {
        return;
    }

    pm_sgp41_sample_t sample;
    if (pm_sgp41_read(latest_temperature_c, latest_humidity_percent,
                      has_latest_sht45, &sample) != ESP_OK) {
        handle_read_failure("SGP41", &sgp41_ready, &sgp41_retry_countdown,
                            &sgp41_consecutive_failures);
        return;
    }

    sgp41_consecutive_failures = 0;
    scalar_add(&frame_acc.voc_index, (float)sample.voc_index);
    if (sample.has_nox) {
        scalar_add(&frame_acc.nox_index, (float)sample.nox_index);
    }
}

static void read_bmp581(void) {
    retry_init(&bmp581_ready, &bmp581_retry_countdown, pm_bmp581_init,
               "BMP581");
    if (!bmp581_ready) {
        return;
    }

    pm_bmp581_sample_t sample;
    if (pm_bmp581_read(&sample) != ESP_OK) {
        if (handle_read_failure("BMP581", &bmp581_ready,
                                &bmp581_retry_countdown,
                                &bmp581_consecutive_failures)) {
            has_latest_pressure = false;
        }
        return;
    }

    bmp581_consecutive_failures = 0;
    latest_pressure_pa = sample.pressure_pa;
    has_latest_pressure = true;
    scalar_add(&frame_acc.pressure_pa, sample.pressure_pa);
}

static void read_scd41(void) {
    retry_init(&scd41_ready, &scd41_retry_countdown, pm_scd41_init, "SCD41");
    if (!scd41_ready) {
        return;
    }

    pm_scd41_sample_t sample;
    esp_err_t err = pm_scd41_read(latest_pressure_pa, has_latest_pressure,
                                  &sample);
    if (err == ESP_ERR_NOT_FINISHED) {
        return;
    }
    if (err != ESP_OK) {
        handle_read_failure("SCD41", &scd41_ready, &scd41_retry_countdown,
                            &scd41_consecutive_failures);
        return;
    }

    scd41_consecutive_failures = 0;
    scalar_add(&frame_acc.co2_ppm, (float)sample.co2_ppm);
}

static void history_add(const sensor_sample_t *sample) {
    if (!sample || !sensor_mutex || !history) {
        return;
    }

    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        latest_sample = *sample;
        latest_valid = true;
        history[history_head] = *sample;
        history_head = (history_head + 1) % SENSOR_HISTORY_CAPACITY;
        if (history_count < SENSOR_HISTORY_CAPACITY) {
            history_count++;
        }
        xSemaphoreGive(sensor_mutex);
    }
}

static void emit_frame(void) {
    sensor_sample_t sample = {0};
    sample.timestamp_ms = frame_tick_count > 0
                              ? (int64_t)(esp_timer_get_time() / 1000)
                              : 0;

    sample.pm1_0 = scalar_avg(&frame_acc.pm1_0);
    sample.pm2_5 = scalar_avg(&frame_acc.pm2_5);
    sample.pm4_0 = scalar_avg(&frame_acc.pm4_0);
    sample.pm10_0 = scalar_avg(&frame_acc.pm10_0);
    sample.nc0_5 = scalar_avg(&frame_acc.nc0_5);
    sample.nc1_0 = scalar_avg(&frame_acc.nc1_0);
    sample.nc2_5 = scalar_avg(&frame_acc.nc2_5);
    sample.nc4_0 = scalar_avg(&frame_acc.nc4_0);
    sample.nc10_0 = scalar_avg(&frame_acc.nc10_0);
    sample.typical_particle_size = scalar_avg(&frame_acc.typical_particle_size);
    sample.pm_count = frame_acc.pm2_5.count;

    sample.temperature_c = scalar_avg(&frame_acc.temperature_c);
    sample.humidity_percent = scalar_avg(&frame_acc.humidity_percent);
    sample.has_temperature = frame_acc.temperature_c.count > 0;
    sample.has_humidity = frame_acc.humidity_percent.count > 0;
    sample.temperature_count = frame_acc.temperature_c.count;
    sample.humidity_count = frame_acc.humidity_percent.count;

    sample.pressure_pa = scalar_avg(&frame_acc.pressure_pa);
    sample.has_pressure = frame_acc.pressure_pa.count > 0;
    sample.pressure_count = frame_acc.pressure_pa.count;

    sample.co2_ppm = (uint16_t)(scalar_avg(&frame_acc.co2_ppm) + 0.5f);
    sample.has_co2 = frame_acc.co2_ppm.count > 0;
    sample.co2_count = frame_acc.co2_ppm.count;

    sample.voc_index = scalar_avg(&frame_acc.voc_index);
    sample.nox_index = scalar_avg(&frame_acc.nox_index);
    sample.has_voc_index = frame_acc.voc_index.count > 0;
    sample.has_nox_index = frame_acc.nox_index.count > 0;
    sample.voc_index_count = frame_acc.voc_index.count;
    sample.nox_index_count = frame_acc.nox_index.count;

    history_add(&sample);
    if (sample_callback) {
        sample_callback(&sample, sample_callback_data);
    }

    memset(&frame_acc, 0, sizeof(frame_acc));
    frame_tick_count = 0;
}

static void sensor_task(void *arg) {
    (void)arg;

    while (1) {
        read_sps30();
        read_sht45();
        read_sgp41();

        if ((frame_tick_count % FRAME_TICKS) == 0) {
            read_bmp581();
            read_scd41();
        }

        frame_tick_count++;
        if (frame_tick_count >= FRAME_TICKS) {
            emit_frame();
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_RAW_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t sensor_service_start(sensor_sample_callback_t on_sample,
                               void *user_data) {
    if (task_handle) {
        return ESP_OK;
    }

    sample_callback = on_sample;
    sample_callback_data = user_data;

    sensor_mutex = xSemaphoreCreateMutex();
    if (!sensor_mutex) {
        return ESP_ERR_NO_MEM;
    }

    history = heap_caps_calloc(SENSOR_HISTORY_CAPACITY, sizeof(sensor_sample_t),
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!history) {
        history = calloc(SENSOR_HISTORY_CAPACITY, sizeof(sensor_sample_t));
    }
    if (!history) {
        ESP_LOGE(TAG, "failed to allocate sensor history");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "history allocated: %d unified frames (%d seconds)",
             SENSOR_HISTORY_CAPACITY,
             (SENSOR_HISTORY_CAPACITY * SENSOR_FRAME_INTERVAL_MS) / 1000);

    BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, "sensor", 6144, NULL,
                                           5, &task_handle, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool sensor_service_get_latest(sensor_sample_t *out) {
    if (!out || !sensor_mutex) {
        return false;
    }

    bool has_sample = false;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *out = latest_sample;
        has_sample = latest_valid;
        xSemaphoreGive(sensor_mutex);
    }
    return has_sample;
}

void sensor_service_get_status(sensor_status_t *out) {
    if (!out) {
        return;
    }

    pm_sps30_status_t sps30_status;
    pm_sps30_get_status(&sps30_status);
    out->state = map_sps30_state(sps30_status.state);
    out->last_error = sps30_status.last_error;
    out->error_count = sps30_status.error_count;
    out->read_count = sps30_status.read_count;
    out->app_read_failures = read_failures;

    pm_sht45_status_t sht45_status;
    pm_sht45_get_status(&sht45_status);
    out->sht45_state = map_sht45_state(sht45_status.state);
    out->sht45_last_error = sht45_status.last_error;
    out->sht45_error_count = sht45_status.error_count;
    out->sht45_read_count = sht45_status.read_count;
    out->sht45_serial = sht45_status.serial;
    out->sht45_detected = sht45_status.detected;

    pm_bmp581_status_t bmp581_status;
    pm_bmp581_get_status(&bmp581_status);
    out->bmp581_state = map_bmp581_state(bmp581_status.state);
    out->bmp581_last_error = bmp581_status.last_error;
    out->bmp581_error_count = bmp581_status.error_count;
    out->bmp581_read_count = bmp581_status.read_count;
    out->bmp581_address = bmp581_status.address;
    out->bmp581_chip_id = bmp581_status.chip_id;
    out->bmp581_detected = bmp581_status.detected;

    pm_scd41_status_t scd41_status;
    pm_scd41_get_status(&scd41_status);
    out->scd41_state = map_scd41_state(scd41_status.state);
    out->scd41_last_error = scd41_status.last_error;
    out->scd41_error_count = scd41_status.error_count;
    out->scd41_read_count = scd41_status.read_count;
    memcpy(out->scd41_serial, scd41_status.serial, sizeof(out->scd41_serial));
    out->scd41_detected = scd41_status.detected;

    pm_sgp41_status_t sgp41_status;
    pm_sgp41_get_status(&sgp41_status);
    out->sgp41_state = map_sgp41_state(sgp41_status.state);
    out->sgp41_last_error = sgp41_status.last_error;
    out->sgp41_error_count = sgp41_status.error_count;
    out->sgp41_read_count = sgp41_status.read_count;
    memcpy(out->sgp41_serial, sgp41_status.serial, sizeof(out->sgp41_serial));
    out->sgp41_self_test_result = sgp41_status.self_test_result;
    out->sgp41_conditioning_remaining_s =
        sgp41_status.conditioning_remaining_s;
    out->sgp41_detected = sgp41_status.detected;
}

uint16_t sensor_service_get_history_count(void) {
    if (!sensor_mutex) {
        return 0;
    }

    uint16_t count = 0;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = history_count;
        xSemaphoreGive(sensor_mutex);
    }
    return count;
}

bool sensor_service_get_history_point(uint16_t order,
                                      sensor_history_point_t *out) {
    if (!out || !sensor_mutex || !history) {
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (order < history_count) {
            uint16_t idx = (history_head + SENSOR_HISTORY_CAPACITY -
                            history_count + order) %
                           SENSOR_HISTORY_CAPACITY;
            sensor_sample_t sample = history[idx];
            out->timestamp_ms = sample.timestamp_ms;
            out->pm1_0 = sample.pm1_0;
            out->pm2_5 = sample.pm2_5;
            out->pm10_0 = sample.pm10_0;
            out->temperature_c = sample.temperature_c;
            out->humidity_percent = sample.humidity_percent;
            out->pressure_pa = sample.pressure_pa;
            out->co2_ppm = sample.co2_ppm;
            out->voc_index = sample.voc_index;
            out->nox_index = sample.nox_index;
            out->has_temperature = sample.has_temperature;
            out->has_humidity = sample.has_humidity;
            out->has_pressure = sample.has_pressure;
            out->has_co2 = sample.has_co2;
            out->has_voc_index = sample.has_voc_index;
            out->has_nox_index = sample.has_nox_index;
            ok = true;
        }
        xSemaphoreGive(sensor_mutex);
    }
    return ok;
}

const char *sensor_state_name(sensor_state_t state) {
    switch (state) {
        case SENSOR_STATE_UNINITIALIZED: return "uninitialized";
        case SENSOR_STATE_INITIALIZING: return "initializing";
        case SENSOR_STATE_CONDITIONING: return "conditioning";
        case SENSOR_STATE_MEASURING: return "measuring";
        case SENSOR_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
