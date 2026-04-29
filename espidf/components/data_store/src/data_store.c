#include "data_store.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "time_service.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "data_store";

#define DATA_PARTITION_LABEL "data"
#define DATA_RECORD_MAGIC 0x51444132u
#define DATA_RECORD_VERSION 4u
#define DATA_RECORD_SIZE 256u
#define DATA_FLASH_SECTOR_SIZE 4096u
#define DATA_TIME_FLAG_VERIFIED   (1u << 0)
#define DATA_TIME_FLAG_RECONCILED (1u << 1)

typedef struct {
    float avg;
    float min;
    float max;
    uint16_t count;
    uint16_t reserved;
} flash_field_stat_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t seq;
    int64_t end_epoch_ms;
    uint16_t frame_count;
    uint16_t field_mask;
    flash_field_stat_t pm2_5;
    flash_field_stat_t pm10_0;
    flash_field_stat_t temperature;
    flash_field_stat_t humidity;
    flash_field_stat_t pressure;
    flash_field_stat_t co2;
    flash_field_stat_t voc_index;
    flash_field_stat_t nox_index;
    uint32_t crc32;
    uint32_t time_flags;
    int64_t end_uptime_ms;
    uint8_t reserved[80];
} flash_data_record_t;

typedef struct {
    double sum;
    float min;
    float max;
    uint32_t count;
} field_accumulator_t;

typedef struct {
    int64_t end_epoch_ms;
    int64_t end_uptime_ms;
    uint32_t frame_count;
    bool time_verified;
    bool time_reconciled;
    field_accumulator_t pm2_5;
    field_accumulator_t pm10_0;
    field_accumulator_t temperature;
    field_accumulator_t humidity;
    field_accumulator_t pressure;
    field_accumulator_t co2;
    field_accumulator_t voc_index;
    field_accumulator_t nox_index;
} data_accumulator_t;

typedef struct {
    uint32_t seq;
    uint32_t slot;
} record_index_t;

_Static_assert(sizeof(flash_field_stat_t) == 16,
               "flash_field_stat_t must stay 16 bytes");
_Static_assert(sizeof(flash_data_record_t) == DATA_RECORD_SIZE,
               "flash_data_record_t must stay one 256-byte flash slot");
_Static_assert((DATA_FLASH_SECTOR_SIZE % DATA_RECORD_SIZE) == 0,
               "record size must divide flash sector size");

static SemaphoreHandle_t store_mutex;
static const esp_partition_t *data_partition;
static data_accumulator_t active;
static record_index_t *record_index;
static uint32_t record_capacity;
static uint32_t record_count;
static uint32_t write_slot;
static uint32_t next_seq = 1;

static void scan_partition_locked(void);

static uint32_t record_crc(const flash_data_record_t *record) {
    return esp_rom_crc32_le(UINT32_MAX, (const uint8_t *)record,
                            offsetof(flash_data_record_t, crc32));
}

static void public_stat_from_flash(const flash_field_stat_t *in,
                                   data_field_stat_t *out) {
    out->avg = in->avg;
    out->min = in->min;
    out->max = in->max;
    out->count = in->count;
}

static void flash_to_public_record(const flash_data_record_t *in,
                                   data_record_t *out) {
    memset(out, 0, sizeof(*out));
    out->seq = in->seq;
    out->end_epoch_ms = in->end_epoch_ms;
    out->end_uptime_ms = in->end_uptime_ms;
    out->frame_count = in->frame_count;
    out->field_mask = in->field_mask;
    out->time_verified = (in->time_flags & DATA_TIME_FLAG_VERIFIED) != 0 ||
                         (in->time_flags == 0 && in->end_epoch_ms > 0);
    out->time_reconciled = (in->time_flags & DATA_TIME_FLAG_RECONCILED) != 0;
    public_stat_from_flash(&in->pm2_5, &out->pm2_5);
    public_stat_from_flash(&in->pm10_0, &out->pm10_0);
    public_stat_from_flash(&in->temperature, &out->temperature);
    public_stat_from_flash(&in->humidity, &out->humidity);
    public_stat_from_flash(&in->pressure, &out->pressure);
    public_stat_from_flash(&in->co2, &out->co2);
    public_stat_from_flash(&in->voc_index, &out->voc_index);
    public_stat_from_flash(&in->nox_index, &out->nox_index);
}

static bool record_valid(const flash_data_record_t *record) {
    return record->magic == DATA_RECORD_MAGIC &&
           record->version == DATA_RECORD_VERSION &&
           record->size == DATA_RECORD_SIZE &&
           record->frame_count > 0 &&
           record->crc32 == record_crc(record);
}

static bool record_erased(const flash_data_record_t *record) {
    const uint8_t *bytes = (const uint8_t *)record;
    for (size_t i = 0; i < sizeof(*record); i++) {
        if (bytes[i] != 0xFF) {
            return false;
        }
    }
    return true;
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
    return record_erased(&record);
}

static int record_index_compare(const void *a, const void *b) {
    const record_index_t *ia = (const record_index_t *)a;
    const record_index_t *ib = (const record_index_t *)b;
    return (ia->seq > ib->seq) - (ia->seq < ib->seq);
}

static uint32_t sector_first_slot(uint32_t slot) {
    uint32_t slot_offset = slot * DATA_RECORD_SIZE;
    return (slot_offset / DATA_FLASH_SECTOR_SIZE) *
           (DATA_FLASH_SECTOR_SIZE / DATA_RECORD_SIZE);
}

static bool slot_in_sector(uint32_t slot, uint32_t first_slot) {
    uint32_t slots_per_sector = DATA_FLASH_SECTOR_SIZE / DATA_RECORD_SIZE;
    return slot >= first_slot && slot < first_slot + slots_per_sector &&
           slot < record_capacity;
}

static uint32_t remove_index_for_sector(uint32_t first_slot) {
    uint32_t removed = 0;
    uint32_t dst = 0;
    for (uint32_t src = 0; src < record_count; src++) {
        if (slot_in_sector(record_index[src].slot, first_slot)) {
            removed++;
        } else {
            record_index[dst++] = record_index[src];
        }
    }
    record_count = dst;
    return removed;
}

static esp_err_t erase_sector_for_slot(uint32_t slot) {
    uint32_t first_slot = sector_first_slot(slot);
    uint32_t erased_valid = remove_index_for_sector(first_slot);
    uint32_t sector_offset = first_slot * DATA_RECORD_SIZE;
    esp_err_t err = esp_partition_erase_range(data_partition, sector_offset,
                                              DATA_FLASH_SECTOR_SIZE);
    if (err != ESP_OK && erased_valid > 0) {
        scan_partition_locked();
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

static int64_t bucket_end_epoch_ms(int64_t epoch_ms) {
    if (epoch_ms <= 0) {
        return DATA_RECORD_WINDOW_MS;
    }
    return (((epoch_ms - 1) / DATA_RECORD_WINDOW_MS) + 1) *
           DATA_RECORD_WINDOW_MS;
}

static int64_t bucket_end_uptime_ms(int64_t uptime_ms) {
    if (uptime_ms <= 0) {
        return DATA_RECORD_WINDOW_MS;
    }
    return (((uptime_ms - 1) / DATA_RECORD_WINDOW_MS) + 1) *
           DATA_RECORD_WINDOW_MS;
}

static bool flash_record_time_verified(const flash_data_record_t *record) {
    return (record->time_flags & DATA_TIME_FLAG_VERIFIED) != 0 ||
           (record->time_flags == 0 && record->end_epoch_ms > 0);
}

static void active_reset(int64_t end_epoch_ms,
                         int64_t end_uptime_ms,
                         bool time_verified,
                         bool time_reconciled) {
    memset(&active, 0, sizeof(active));
    active.end_epoch_ms = end_epoch_ms;
    active.end_uptime_ms = end_uptime_ms;
    active.time_verified = time_verified;
    active.time_reconciled = time_reconciled;
}

static void field_add(field_accumulator_t *acc, float value) {
    if (acc->count == 0) {
        acc->min = value;
        acc->max = value;
    } else {
        if (value < acc->min) {
            acc->min = value;
        }
        if (value > acc->max) {
            acc->max = value;
        }
    }
    acc->sum += value;
    acc->count++;
}

static uint16_t field_mask_for_active(void) {
    uint16_t mask = 0;
    if (active.pm2_5.count > 0) {
        mask |= DATA_FIELD_PM25;
    }
    if (active.pm10_0.count > 0) {
        mask |= DATA_FIELD_PM10;
    }
    if (active.temperature.count > 0) {
        mask |= DATA_FIELD_TEMPERATURE;
    }
    if (active.humidity.count > 0) {
        mask |= DATA_FIELD_HUMIDITY;
    }
    if (active.pressure.count > 0) {
        mask |= DATA_FIELD_PRESSURE;
    }
    if (active.co2.count > 0) {
        mask |= DATA_FIELD_CO2;
    }
    if (active.voc_index.count > 0) {
        mask |= DATA_FIELD_VOC_INDEX;
    }
    if (active.nox_index.count > 0) {
        mask |= DATA_FIELD_NOX_INDEX;
    }
    return mask;
}

static flash_field_stat_t flash_stat_from_acc(const field_accumulator_t *acc) {
    flash_field_stat_t out = {0};
    if (acc->count > 0) {
        out.avg = (float)(acc->sum / acc->count);
        out.min = acc->min;
        out.max = acc->max;
        out.count = (acc->count > UINT16_MAX) ? UINT16_MAX : (uint16_t)acc->count;
    }
    return out;
}

static data_field_stat_t public_stat_from_acc(const field_accumulator_t *acc) {
    data_field_stat_t out = {0};
    if (acc->count > 0) {
        out.avg = (float)(acc->sum / acc->count);
        out.min = acc->min;
        out.max = acc->max;
        out.count = (acc->count > UINT16_MAX) ? UINT16_MAX : (uint16_t)acc->count;
    }
    return out;
}

static void append_record_locked(void) {
    if (!data_partition || active.frame_count == 0 || record_capacity == 0) {
        return;
    }
    uint16_t field_mask = field_mask_for_active();
    if (field_mask == 0) {
        return;
    }

    flash_data_record_t record = {
        .magic = DATA_RECORD_MAGIC,
        .version = DATA_RECORD_VERSION,
        .size = DATA_RECORD_SIZE,
        .seq = next_seq++,
        .end_epoch_ms = active.end_epoch_ms,
        .end_uptime_ms = active.end_uptime_ms,
        .frame_count = (active.frame_count > UINT16_MAX)
                           ? UINT16_MAX
                           : (uint16_t)active.frame_count,
        .field_mask = field_mask,
        .pm2_5 = flash_stat_from_acc(&active.pm2_5),
        .pm10_0 = flash_stat_from_acc(&active.pm10_0),
        .temperature = flash_stat_from_acc(&active.temperature),
        .humidity = flash_stat_from_acc(&active.humidity),
        .pressure = flash_stat_from_acc(&active.pressure),
        .co2 = flash_stat_from_acc(&active.co2),
        .voc_index = flash_stat_from_acc(&active.voc_index),
        .nox_index = flash_stat_from_acc(&active.nox_index),
        .time_flags = (active.time_verified ? DATA_TIME_FLAG_VERIFIED : 0) |
                      (active.time_reconciled ? DATA_TIME_FLAG_RECONCILED : 0),
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
    if (record_index && record_count < record_capacity) {
        record_index[record_count].seq = record.seq;
        record_index[record_count].slot =
            (write_slot + record_capacity - 1) % record_capacity;
        record_count++;
    }

    ESP_LOGI(TAG, "data saved: seq=%lu end_epoch=%lld end_uptime=%lld frames=%u fields=0x%x verified=%d",
             (unsigned long)record.seq,
             (long long)record.end_epoch_ms,
             (long long)record.end_uptime_ms,
             record.frame_count,
             record.field_mask,
             flash_record_time_verified(&record) ? 1 : 0);
}

static bool append_flash_record_locked(flash_data_record_t *record) {
    if (!data_partition || !record || record_capacity == 0) {
        return false;
    }

    record->seq = next_seq++;
    record->crc32 = record_crc(record);

    uint32_t slot = write_slot;
    esp_err_t err = slot_write(slot, record);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "data record rewrite failed at slot %lu: %s",
                 (unsigned long)slot,
                 esp_err_to_name(err));
        return false;
    }

    write_slot = (write_slot + 1) % record_capacity;
    if (record_index && record_count < record_capacity) {
        record_index[record_count].seq = record->seq;
        record_index[record_count].slot = slot;
        record_count++;
    }
    return true;
}

static void remove_index_at(uint32_t index) {
    if (index >= record_count) {
        return;
    }
    for (uint32_t i = index + 1; i < record_count; i++) {
        record_index[i - 1] = record_index[i];
    }
    record_count--;
}

static bool remove_index_for_slot_exact(uint32_t slot) {
    for (uint32_t i = 0; i < record_count; i++) {
        if (record_index[i].slot == slot) {
            remove_index_at(i);
            return true;
        }
    }
    return false;
}

static void invalidate_slot(uint32_t slot) {
    uint32_t zero = 0;
    esp_err_t err = esp_partition_write(data_partition,
                                        slot * DATA_RECORD_SIZE,
                                        &zero,
                                        sizeof(zero));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to invalidate old data slot %lu: %s",
                 (unsigned long)slot,
                 esp_err_to_name(err));
    }
}

static void reconcile_unverified_records_locked(void) {
    uint32_t converted = 0;
    for (uint32_t i = 0; i < record_count;) {
        uint32_t slot = record_index[i].slot;
        flash_data_record_t record;
        if (!slot_read(slot, &record) || !record_valid(&record)) {
            remove_index_at(i);
            continue;
        }
        if (flash_record_time_verified(&record) || record.end_uptime_ms <= 0) {
            i++;
            continue;
        }

        int64_t estimated_epoch_ms = 0;
        if (!time_service_uptime_to_epoch_ms(record.end_uptime_ms,
                                             &estimated_epoch_ms)) {
            break;
        }

        flash_data_record_t corrected = record;
        corrected.end_epoch_ms = bucket_end_epoch_ms(estimated_epoch_ms);
        corrected.time_flags = DATA_TIME_FLAG_VERIFIED |
                               DATA_TIME_FLAG_RECONCILED;
        if (!append_flash_record_locked(&corrected)) {
            break;
        }

        invalidate_slot(slot);
        if (!remove_index_for_slot_exact(slot)) {
            continue;
        }
        converted++;
    }

    if (converted > 0) {
        qsort(record_index, record_count, sizeof(record_index[0]),
              record_index_compare);
        ESP_LOGI(TAG, "reconciled %lu unverified data records after time sync",
                 (unsigned long)converted);
    }
}

static void scan_partition_locked(void) {
    record_count = 0;
    write_slot = 0;
    next_seq = 1;

    uint32_t max_seq = 0;
    uint32_t max_slot = 0;
    for (uint32_t slot = 0; slot < record_capacity; slot++) {
        flash_data_record_t record;
        if (!slot_read(slot, &record)) {
            continue;
        }
        if (!record_valid(&record)) {
            continue;
        }
        record_count++;
        record_index[record_count - 1].seq = record.seq;
        record_index[record_count - 1].slot = slot;
        if (record.seq >= max_seq) {
            max_seq = record.seq;
            max_slot = slot;
        }
    }

    if (record_count > 0) {
        qsort(record_index, record_count, sizeof(record_index[0]),
              record_index_compare);
        next_seq = max_seq + 1;
        write_slot = (max_slot + 1) % record_capacity;
        for (uint32_t i = 0; i < record_capacity; i++) {
            uint32_t slot = (max_slot + 1 + i) % record_capacity;
            if (slot_is_erased(slot)) {
                write_slot = slot;
                break;
            }
        }
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

    record_index = heap_caps_malloc(record_capacity * sizeof(record_index[0]),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!record_index) {
        record_index = malloc(record_capacity * sizeof(record_index[0]));
    }
    if (!record_index) {
        ESP_LOGE(TAG, "failed to allocate data record index");
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(store_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    scan_partition_locked();
    active_reset(0, 0, false, false);
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

    int64_t sample_epoch_ms = 0;
    bool time_verified = time_service_uptime_to_epoch_ms(sample->timestamp_ms,
                                                        &sample_epoch_ms);
    if (time_verified) {
        reconcile_unverified_records_locked();
        if (active.frame_count > 0 && !active.time_verified &&
            active.end_uptime_ms > 0) {
            int64_t active_epoch_ms = 0;
            if (time_service_uptime_to_epoch_ms(active.end_uptime_ms,
                                                &active_epoch_ms)) {
                active.end_epoch_ms = bucket_end_epoch_ms(active_epoch_ms);
                active.time_verified = true;
                active.time_reconciled = true;
            }
        }
    }

    int64_t bucket_end = time_verified ? bucket_end_epoch_ms(sample_epoch_ms) : 0;
    int64_t uptime_bucket_end = bucket_end_uptime_ms(sample->timestamp_ms);
    bool bucket_changed = false;
    if (active.frame_count == 0) {
        active_reset(bucket_end, uptime_bucket_end, time_verified, false);
    } else if (time_verified && active.time_verified &&
               bucket_end < active.end_epoch_ms) {
        ESP_LOGW(TAG, "time moved backwards, dropping active partial bucket");
        active_reset(bucket_end, uptime_bucket_end, true, false);
    } else if (time_verified && active.time_verified) {
        bucket_changed = bucket_end != active.end_epoch_ms;
    } else if (!time_verified && !active.time_verified) {
        bucket_changed = uptime_bucket_end != active.end_uptime_ms;
    } else if (time_verified != active.time_verified) {
        bucket_changed = true;
    }

    if (bucket_changed) {
        append_record_locked();
        active_reset(bucket_end, uptime_bucket_end, time_verified, false);
    }

    active.frame_count++;
    if (sample->pm_count > 0) {
        field_add(&active.pm2_5, sample->pm2_5);
        field_add(&active.pm10_0, sample->pm10_0);
    }
    if (sample->has_temperature) {
        field_add(&active.temperature, sample->temperature_c);
    }
    if (sample->has_humidity) {
        field_add(&active.humidity, sample->humidity_percent);
    }
    if (sample->has_pressure) {
        field_add(&active.pressure, sample->pressure_pa);
    }
    if (sample->has_co2) {
        field_add(&active.co2, (float)sample->co2_ppm);
    }
    if (sample->has_voc_index) {
        field_add(&active.voc_index, sample->voc_index);
    }
    if (sample->has_nox_index) {
        field_add(&active.nox_index, sample->nox_index);
    }

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
    out->end_epoch_ms = active.end_epoch_ms;
    out->end_uptime_ms = active.end_uptime_ms;
    out->frame_count = (active.frame_count > UINT16_MAX)
                           ? UINT16_MAX
                           : (uint16_t)active.frame_count;
    out->field_mask = field_mask_for_active();
    out->time_verified = active.time_verified;
    out->time_reconciled = active.time_reconciled;
    out->pm2_5 = public_stat_from_acc(&active.pm2_5);
    out->pm10_0 = public_stat_from_acc(&active.pm10_0);
    out->temperature = public_stat_from_acc(&active.temperature);
    out->humidity = public_stat_from_acc(&active.humidity);
    out->pressure = public_stat_from_acc(&active.pressure);
    out->co2 = public_stat_from_acc(&active.co2);
    out->voc_index = public_stat_from_acc(&active.voc_index);
    out->nox_index = public_stat_from_acc(&active.nox_index);
}

bool data_store_get_record_locked(uint32_t order, data_record_t *out) {
    if (!out || order >= record_count || record_capacity == 0) {
        return false;
    }

    uint32_t slot = record_index[order].slot;
    flash_data_record_t record;
    if (!slot_read(slot, &record) || !record_valid(&record)) {
        return false;
    }

    flash_to_public_record(&record, out);
    return true;
}

void data_store_get_summary(uint32_t *stored, uint32_t *active_frames,
                            float *active_pm25) {
    if (stored) {
        *stored = 0;
    }
    if (active_frames) {
        *active_frames = 0;
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
    if (active_frames) {
        *active_frames = active.frame_count;
    }
    if (active_pm25 && active.pm2_5.count > 0) {
        *active_pm25 = (float)(active.pm2_5.sum / active.pm2_5.count);
    }

    data_store_unlock();
}
