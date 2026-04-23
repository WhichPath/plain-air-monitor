#include "sensor_service.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pm_sps30.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sensor_service";

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

static sensor_state_t map_sps30_state(pm_sps30_state_t state) {
    switch (state) {
        case PM_SPS30_STATE_INITIALIZING: return SENSOR_STATE_INITIALIZING;
        case PM_SPS30_STATE_MEASURING: return SENSOR_STATE_MEASURING;
        case PM_SPS30_STATE_ERROR: return SENSOR_STATE_ERROR;
        case PM_SPS30_STATE_UNINITIALIZED:
        default: return SENSOR_STATE_UNINITIALIZED;
    }
}

static void sample_from_sps30(sensor_sample_t *out, const pm_sps30_sample_t *in) {
    out->pm1_0 = in->pm1_0;
    out->pm2_5 = in->pm2_5;
    out->pm4_0 = in->pm4_0;
    out->pm10_0 = in->pm10_0;
    out->nc0_5 = in->nc0_5;
    out->nc1_0 = in->nc1_0;
    out->nc2_5 = in->nc2_5;
    out->nc4_0 = in->nc4_0;
    out->nc10_0 = in->nc10_0;
    out->typical_particle_size = in->typical_particle_size;
    out->timestamp_ms = in->timestamp_ms;
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

static void sensor_task(void *arg) {
    (void)arg;
    int consecutive_failures = 0;

    while (1) {
        if (pm_sps30_init() == ESP_OK) {
            consecutive_failures = 0;
            break;
        }
        consecutive_failures++;
        ESP_LOGW(TAG, "SPS30 init retry in 5s (failures=%d)", consecutive_failures);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }

    while (1) {
        pm_sps30_sample_t raw_sample;
        esp_err_t err = pm_sps30_read(&raw_sample);
        if (err == ESP_OK) {
            sensor_sample_t sample;
            sample_from_sps30(&sample, &raw_sample);
            consecutive_failures = 0;
            history_add(&sample);
            if (sample_callback) {
                sample_callback(&sample, sample_callback_data);
            }
        } else {
            read_failures++;
            consecutive_failures++;
            if (consecutive_failures >= 10) {
                ESP_LOGW(TAG, "too many SPS30 read failures, reinitializing");
                pm_sps30_stop();
                while (pm_sps30_init() != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
                consecutive_failures = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SENSOR_SAMPLE_INTERVAL_MS));
    }
}

esp_err_t sensor_service_start(sensor_sample_callback_t on_sample, void *user_data) {
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
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!history) {
        history = calloc(SENSOR_HISTORY_CAPACITY, sizeof(sensor_sample_t));
    }
    if (!history) {
        ESP_LOGE(TAG, "failed to allocate sensor history");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "history allocated: %d samples (%d seconds)",
             SENSOR_HISTORY_CAPACITY,
             (SENSOR_HISTORY_CAPACITY * SENSOR_SAMPLE_INTERVAL_MS) / 1000);

    BaseType_t ok = xTaskCreatePinnedToCore(sensor_task, "sensor", 4096, NULL, 5,
                                           &task_handle, 1);
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

bool sensor_service_get_history_point(uint16_t order, sensor_history_point_t *out) {
    if (!out || !sensor_mutex || !history) {
        return false;
    }

    bool ok = false;
    if (xSemaphoreTake(sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (order < history_count) {
            uint16_t idx = (history_head + SENSOR_HISTORY_CAPACITY - history_count + order) %
                           SENSOR_HISTORY_CAPACITY;
            sensor_sample_t sample = history[idx];
            out->timestamp_ms = sample.timestamp_ms;
            out->pm1_0 = sample.pm1_0;
            out->pm2_5 = sample.pm2_5;
            out->pm10_0 = sample.pm10_0;
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
        case SENSOR_STATE_MEASURING: return "measuring";
        case SENSOR_STATE_ERROR: return "error";
        default: return "unknown";
    }
}
