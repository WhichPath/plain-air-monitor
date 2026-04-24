#include "data_store.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include <stddef.h>
#include <string.h>

static const char *TAG = "data_store";

#define DATA_PARTITION_LABEL "data"
#define DATA_RECORD_MAGIC 0x504D4431u
#define DATA_RECORD_VERSION 1u
#define DATA_RECORD_SIZE 128u
#define DATA_FLASH_SECTOR_SIZE 4096u

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
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
    uint32_t crc32;
    uint8_t reserved[36];
} flash_data_record_t;

typedef struct {
    int64_t start_ms;
    int64_t last_sample_ms;
    int64_t elapsed_ms;
    uint32_t count;
    double pm2_5_sum;
    double pm10_0_sum;
    float pm2_5_min;
    float pm2_5_max;
    float pm10_0_min;
    float pm10_0_max;
} data_accumulator_t;

_Static_assert(sizeof(flash_data_record_t) == DATA_RECORD_SIZE,
               "flash_data_record_t must stay one 128-byte flash slot");
_Static_assert((DATA_FLASH_SECTOR_SIZE % DATA_RECORD_SIZE) == 0,
               "record size must divide flash sector size");

static SemaphoreHandle_t store_mutex;
static const esp_partition_t *data_partition;
static data_accumulator_t active;
static uint32_t record_capacity;
static uint32_t record_count;
static uint32_t write_slot;
static uint32_t next_seq = 1;

static uint32_t record_crc(const flash_data_record_t *record) {
    return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)record,
                            offsetof(flash_data_record_t, crc32));
}

static void flash_to_public_record(const flash_data_record_t *in, data_record_t *out) {
    memset(out, 0, sizeof(*out));
    out->seq = in->seq;
    out->start_ms = in->start_ms;
    out->end_ms = in->end_ms;
    out->duration_ms = in->duration_ms;
    out->sample_count = in->sample_count;
    out->field_mask = in->field_mask;
    out->pm2_5_avg = in->pm2_5_avg;
    out->pm2_5_min = in->pm2_5_min;
    out->pm2_5_max = in->pm2_5_max;
    out->pm10_0_avg = in->pm10_0_avg;
    out->pm10_0_min = in->pm10_0_min;
    out->pm10_0_max = in->pm10_0_max;
    out->temperature_avg = in->temperature_avg;
    out->temperature_min = in->temperature_min;
    out->temperature_max = in->temperature_max;
    out->humidity_avg = in->humidity_avg;
    out->humidity_min = in->humidity_min;
    out->humidity_max = in->humidity_max;
}

static bool record_valid(const flash_data_record_t *record) {
    return record->magic == DATA_RECORD_MAGIC &&
           record->version == DATA_RECORD_VERSION &&
           record->size == DATA_RECORD_SIZE &&
           record->sample_count > 0 &&
           record->crc32 == record_crc(record);
}

static bool slot_read(uint32_t slot, flash_data_record_t *record) {
    if (!data_partition || slot >= record_capacity) {
        return false;
    }

    esp_err_t err = esp_partition_read(data_partition,
                                       slot * DATA_RECORD_SIZE,
                                       record,
                                       sizeof(*record));
    return err == ESP_OK;
}

static bool slot_is_erased(uint32_t slot) {
    flash_data_record_t record;
    if (!slot_read(slot, &record)) {
        return false;
    }

    const uint8_t *bytes = (const uint8_t *)&record;
    for (size_t i = 0; i < sizeof(record); i++) {
        if (bytes[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static uint32_t sector_first_slot(uint32_t slot) {
    uint32_t slot_offset = slot * DATA_RECORD_SIZE;
    return (slot_offset / DATA_FLASH_SECTOR_SIZE) *
           (DATA_FLASH_SECTOR_SIZE / DATA_RECORD_SIZE);
}

static uint32_t count_valid_in_sector(uint32_t first_slot) {
    uint32_t slots_per_sector = DATA_FLASH_SECTOR_SIZE / DATA_RECORD_SIZE;
    uint32_t count = 0;
    for (uint32_t i = 0; i < slots_per_sector; i++) {
        uint32_t slot = first_slot + i;
        if (slot >= record_capacity) {
            break;
        }
        flash_data_record_t record;
        if (slot_read(slot, &record) && record_valid(&record)) {
            count++;
        }
    }
    return count;
}

static esp_err_t erase_sector_for_slot(uint32_t slot) {
    uint32_t first_slot = sector_first_slot(slot);
    uint32_t erased_valid = count_valid_in_sector(first_slot);
    uint32_t sector_offset = first_slot * DATA_RECORD_SIZE;
    esp_err_t err = esp_partition_erase_range(data_partition, sector_offset,
                                              DATA_FLASH_SECTOR_SIZE);
    if (err == ESP_OK) {
        record_count = (record_count > erased_valid) ? record_count - erased_valid : 0;
    }
    return err;
}

static esp_err_t slot_write(uint32_t slot, const flash_data_record_t *record) {
    if (!slot_is_erased(slot)) {
        esp_err_t erase_err = erase_sector_for_slot(slot);
        if (erase_err != ESP_OK) {
            return erase_err;
        }
    }

    return esp_partition_write(data_partition,
                               slot * DATA_RECORD_SIZE,
                               record,
                               sizeof(*record));
}

static void active_reset(int64_t start_ms) {
    memset(&active, 0, sizeof(active));
    active.start_ms = start_ms;
    active.last_sample_ms = start_ms;
}

static void append_record_locked(int64_t end_ms) {
    if (!data_partition || active.count == 0 || record_capacity == 0) {
        return;
    }

    flash_data_record_t record = {
        .magic = DATA_RECORD_MAGIC,
        .version = DATA_RECORD_VERSION,
        .size = DATA_RECORD_SIZE,
        .seq = next_seq++,
        .start_ms = active.start_ms,
        .end_ms = (active.elapsed_ms > 0) ? active.start_ms + active.elapsed_ms : end_ms,
        .duration_ms = (uint32_t)active.elapsed_ms,
        .sample_count = (active.count > UINT16_MAX) ? UINT16_MAX : (uint16_t)active.count,
        .field_mask = DATA_FIELD_PM25 | DATA_FIELD_PM10,
        .pm2_5_avg = (float)(active.pm2_5_sum / active.count),
        .pm2_5_min = active.pm2_5_min,
        .pm2_5_max = active.pm2_5_max,
        .pm10_0_avg = (float)(active.pm10_0_sum / active.count),
        .pm10_0_min = active.pm10_0_min,
        .pm10_0_max = active.pm10_0_max,
    };
    record.crc32 = record_crc(&record);

    esp_err_t err = slot_write(write_slot, &record);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "data record write failed at slot %lu: %s",
                 (unsigned long)write_slot,
                 esp_err_to_name(err));
        return;
    }

    write_slot = (write_slot + 1) % record_capacity;
    if (record_count < record_capacity) {
        record_count++;
    }

    ESP_LOGI(TAG, "data saved: seq=%lu samples=%u PM2.5 avg/min/max=%.2f/%.2f/%.2f PM10 avg/min/max=%.2f/%.2f/%.2f",
             (unsigned long)record.seq,
             record.sample_count,
             record.pm2_5_avg,
             record.pm2_5_min,
             record.pm2_5_max,
             record.pm10_0_avg,
             record.pm10_0_min,
             record.pm10_0_max);

    active_reset(end_ms);
}

static void scan_partition_locked(void) {
    record_count = 0;
    write_slot = 0;
    next_seq = 1;

    uint32_t max_seq = 0;
    uint32_t max_slot = 0;
    for (uint32_t slot = 0; slot < record_capacity; slot++) {
        flash_data_record_t record;
        if (!slot_read(slot, &record) || !record_valid(&record)) {
            continue;
        }
        record_count++;
        if (record.seq >= max_seq) {
            max_seq = record.seq;
            max_slot = slot;
        }
    }

    if (record_count > 0) {
        next_seq = max_seq + 1;
        write_slot = (max_slot + 1) % record_capacity;
    }
}

esp_err_t data_store_init(void) {
    store_mutex = xSemaphoreCreateMutex();
    if (!store_mutex) {
        return ESP_ERR_NO_MEM;
    }

    data_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_ANY,
                                              DATA_PARTITION_LABEL);
    if (!data_partition) {
        ESP_LOGE(TAG, "data partition \"%s\" not found", DATA_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    record_capacity = data_partition->size / DATA_RECORD_SIZE;
    if (record_capacity == 0) {
        ESP_LOGE(TAG, "data partition too small: %lu bytes",
                 (unsigned long)data_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (xSemaphoreTake(store_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    scan_partition_locked();
    active_reset(0);
    xSemaphoreGive(store_mutex);

    ESP_LOGI(TAG, "data partition ready: offset=0x%lx size=%lu records=%lu/%lu next_seq=%lu",
             (unsigned long)data_partition->address,
             (unsigned long)data_partition->size,
             (unsigned long)record_count,
             (unsigned long)record_capacity,
             (unsigned long)next_seq);
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

    if (active.count == 0) {
        active_reset(sample->timestamp_ms);
        active.pm2_5_min = sample->pm2_5;
        active.pm2_5_max = sample->pm2_5;
        active.pm10_0_min = sample->pm10_0;
        active.pm10_0_max = sample->pm10_0;
    } else {
        int64_t delta_ms = sample->timestamp_ms - active.last_sample_ms;
        if (delta_ms < 0 || delta_ms > DATA_RECORD_WINDOW_MS) {
            delta_ms = SENSOR_SAMPLE_INTERVAL_MS;
            active.start_ms = (sample->timestamp_ms >= active.elapsed_ms) ?
                              sample->timestamp_ms - active.elapsed_ms : 0;
        }
        if (active.elapsed_ms + delta_ms >= DATA_RECORD_WINDOW_MS) {
            append_record_locked(sample->timestamp_ms);
            active_reset(sample->timestamp_ms);
            active.pm2_5_min = sample->pm2_5;
            active.pm2_5_max = sample->pm2_5;
            active.pm10_0_min = sample->pm10_0;
            active.pm10_0_max = sample->pm10_0;
        } else {
            active.elapsed_ms += delta_ms;
            active.last_sample_ms = sample->timestamp_ms;
        }
    }

    active.count++;
    active.pm2_5_sum += sample->pm2_5;
    active.pm10_0_sum += sample->pm10_0;
    if (sample->pm2_5 < active.pm2_5_min) active.pm2_5_min = sample->pm2_5;
    if (sample->pm2_5 > active.pm2_5_max) active.pm2_5_max = sample->pm2_5;
    if (sample->pm10_0 < active.pm10_0_min) active.pm10_0_min = sample->pm10_0;
    if (sample->pm10_0 > active.pm10_0_max) active.pm10_0_max = sample->pm10_0;

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

uint32_t data_store_record_count_locked(void) {
    return record_count;
}

uint32_t data_store_record_capacity_locked(void) {
    return record_capacity;
}

void data_store_get_active_locked(data_active_t *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->start_ms = active.start_ms;
    out->elapsed_ms = (uint32_t)active.elapsed_ms;
    out->sample_count = (active.count > UINT16_MAX) ? UINT16_MAX : (uint16_t)active.count;
    out->field_mask = DATA_FIELD_PM25 | DATA_FIELD_PM10;
    if (active.count > 0) {
        out->pm2_5_avg = (float)(active.pm2_5_sum / active.count);
        out->pm2_5_min = active.pm2_5_min;
        out->pm2_5_max = active.pm2_5_max;
        out->pm10_0_avg = (float)(active.pm10_0_sum / active.count);
        out->pm10_0_min = active.pm10_0_min;
        out->pm10_0_max = active.pm10_0_max;
    }
}

bool data_store_get_record_locked(uint32_t order, data_record_t *out) {
    if (!out || order >= record_count || record_capacity == 0) {
        return false;
    }

    uint32_t oldest_slot = (write_slot + record_capacity - record_count) %
                           record_capacity;
    uint32_t slot = (oldest_slot + order) % record_capacity;
    flash_data_record_t record;
    if (!slot_read(slot, &record) || !record_valid(&record)) {
        return false;
    }

    flash_to_public_record(&record, out);
    return true;
}

void data_store_get_summary(uint32_t *stored, uint32_t *active_samples, float *active_pm25) {
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
        *stored = record_count;
    }
    if (active_samples) {
        *active_samples = active.count;
    }
    if (active_pm25 && active.count > 0) {
        *active_pm25 = (float)(active.pm2_5_sum / active.count);
    }

    data_store_unlock();
}
