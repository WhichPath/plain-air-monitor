#include "data_store.h"

#include "esp_log.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "data_store";

#define DATA_NVS_NAMESPACE "pm_hourly"
#define DATA_NVS_KEY "records"
#define DATA_MAGIC 0x504D4831u
#define DATA_VERSION 1u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t head;
    uint16_t reserved;
    uint32_t next_seq;
    hourly_record_t records[HOURLY_RECORD_CAPACITY];
} hourly_store_t;

typedef struct {
    int64_t start_ms;
    uint32_t count;
    double pm1_0;
    double pm2_5;
    double pm4_0;
    double pm10_0;
} hourly_accumulator_t;

static SemaphoreHandle_t store_mutex;
static hourly_store_t hourly_store;
static hourly_accumulator_t hourly_acc;
static nvs_handle_t hourly_nvs;
static bool hourly_nvs_ready;

static void store_reset(void) {
    memset(&hourly_store, 0, sizeof(hourly_store));
    hourly_store.magic = DATA_MAGIC;
    hourly_store.version = DATA_VERSION;
    hourly_store.next_seq = 1;
}

static esp_err_t save_locked(void) {
    if (!hourly_nvs_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_set_blob(hourly_nvs, DATA_NVS_KEY, &hourly_store,
                                 sizeof(hourly_store));
    if (err == ESP_OK) {
        err = nvs_commit(hourly_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hourly NVS save failed: %s", esp_err_to_name(err));
    }
    return err;
}

static void finalize_locked(int64_t end_ms) {
    if (hourly_acc.count == 0) {
        return;
    }

    hourly_record_t rec = {
        .seq = hourly_store.next_seq++,
        .start_ms = hourly_acc.start_ms,
        .end_ms = end_ms,
        .sample_count = hourly_acc.count,
        .pm1_0 = (float)(hourly_acc.pm1_0 / hourly_acc.count),
        .pm2_5 = (float)(hourly_acc.pm2_5 / hourly_acc.count),
        .pm4_0 = (float)(hourly_acc.pm4_0 / hourly_acc.count),
        .pm10_0 = (float)(hourly_acc.pm10_0 / hourly_acc.count),
    };

    hourly_store.records[hourly_store.head] = rec;
    hourly_store.head = (hourly_store.head + 1) % HOURLY_RECORD_CAPACITY;
    if (hourly_store.count < HOURLY_RECORD_CAPACITY) {
        hourly_store.count++;
    }

    ESP_LOGI(TAG, "hourly saved: seq=%lu samples=%lu PM2.5=%.2f PM10=%.2f",
             (unsigned long)rec.seq,
             (unsigned long)rec.sample_count,
             rec.pm2_5,
             rec.pm10_0);

    save_locked();
    memset(&hourly_acc, 0, sizeof(hourly_acc));
}

esp_err_t data_store_init(void) {
    store_mutex = xSemaphoreCreateMutex();
    if (!store_mutex) {
        return ESP_ERR_NO_MEM;
    }

    store_reset();
    esp_err_t err = nvs_open(DATA_NVS_NAMESPACE, NVS_READWRITE, &hourly_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hourly NVS open failed: %s", esp_err_to_name(err));
        return err;
    }
    hourly_nvs_ready = true;

    size_t len = sizeof(hourly_store);
    err = nvs_get_blob(hourly_nvs, DATA_NVS_KEY, &hourly_store, &len);
    if (err != ESP_OK || len != sizeof(hourly_store) ||
        hourly_store.magic != DATA_MAGIC ||
        hourly_store.version != DATA_VERSION ||
        hourly_store.count > HOURLY_RECORD_CAPACITY ||
        hourly_store.head >= HOURLY_RECORD_CAPACITY) {
        store_reset();
        save_locked();
        ESP_LOGI(TAG, "hourly store initialized (%d records)", HOURLY_RECORD_CAPACITY);
    } else {
        ESP_LOGI(TAG, "hourly store loaded: %u records", hourly_store.count);
    }

    return ESP_OK;
}

void data_store_add_sample(const sensor_sample_t *sample, void *user_data) {
    (void)user_data;
    if (!sample || !store_mutex) {
        return;
    }

    if (xSemaphoreTake(store_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return;
    }

    if (hourly_acc.count > 0 &&
        sample->timestamp_ms - hourly_acc.start_ms >= HOURLY_WINDOW_MS) {
        finalize_locked(sample->timestamp_ms);
    }

    if (hourly_acc.count == 0) {
        hourly_acc.start_ms = sample->timestamp_ms;
    }

    hourly_acc.count++;
    hourly_acc.pm1_0 += sample->pm1_0;
    hourly_acc.pm2_5 += sample->pm2_5;
    hourly_acc.pm4_0 += sample->pm4_0;
    hourly_acc.pm10_0 += sample->pm10_0;

    xSemaphoreGive(store_mutex);
}

bool data_store_lock(TickType_t timeout_ticks) {
    return store_mutex && xSemaphoreTake(store_mutex, timeout_ticks) == pdTRUE;
}

void data_store_unlock(void) {
    if (store_mutex) {
        xSemaphoreGive(store_mutex);
    }
}

uint16_t data_store_hourly_count_locked(void) {
    return hourly_store.count;
}

void data_store_get_active_locked(hourly_active_t *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->start_ms = hourly_acc.start_ms;
    out->sample_count = hourly_acc.count;
    if (hourly_acc.count > 0) {
        out->pm1_0 = (float)(hourly_acc.pm1_0 / hourly_acc.count);
        out->pm2_5 = (float)(hourly_acc.pm2_5 / hourly_acc.count);
        out->pm4_0 = (float)(hourly_acc.pm4_0 / hourly_acc.count);
        out->pm10_0 = (float)(hourly_acc.pm10_0 / hourly_acc.count);
    }
}

bool data_store_get_hourly_record_locked(uint16_t order, hourly_record_t *out) {
    if (!out || order >= hourly_store.count) {
        return false;
    }

    uint16_t idx = (hourly_store.head + HOURLY_RECORD_CAPACITY -
                    hourly_store.count + order) % HOURLY_RECORD_CAPACITY;
    *out = hourly_store.records[idx];
    return true;
}

void data_store_get_summary(uint16_t *stored, uint32_t *active_samples, float *active_pm25) {
    if (stored) {
        *stored = 0;
    }
    if (active_samples) {
        *active_samples = 0;
    }
    if (active_pm25) {
        *active_pm25 = 0.0f;
    }

    if (!data_store_lock(pdMS_TO_TICKS(100))) {
        return;
    }

    if (stored) {
        *stored = hourly_store.count;
    }
    if (active_samples) {
        *active_samples = hourly_acc.count;
    }
    if (active_pm25 && hourly_acc.count > 0) {
        *active_pm25 = (float)(hourly_acc.pm2_5 / hourly_acc.count);
    }

    data_store_unlock();
}
