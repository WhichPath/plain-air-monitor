#include "time_service.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "time_service";

static bool s_started;
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_synced;
static int64_t s_boot_epoch_ms;
static uint32_t s_sync_count;

static int64_t current_epoch_ms(void) {
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + ((int64_t)tv.tv_usec / 1000LL);
}

static void update_boot_epoch(void) {
    int64_t uptime_ms = esp_timer_get_time() / 1000LL;
    int64_t epoch_ms = current_epoch_ms();
    portENTER_CRITICAL(&s_time_lock);
    s_boot_epoch_ms = epoch_ms - uptime_ms;
    s_synced = true;
    s_sync_count++;
    portEXIT_CRITICAL(&s_time_lock);
}

static void sync_cb(struct timeval *tv) {
    (void)tv;
    update_boot_epoch();
    time_service_snapshot_t snapshot;
    time_service_get_snapshot(&snapshot);
    ESP_LOGI(TAG, "time synced: boot_epoch_ms=%lld now_epoch_ms=%lld",
             (long long)snapshot.boot_epoch_ms,
             (long long)snapshot.now_epoch_ms);
}

void time_service_start(void) {
    if (s_started) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_set_time_sync_notification_cb(sync_cb);
    esp_sntp_init();
    s_started = true;

    ESP_LOGI(TAG, "SNTP started for Asia/Shanghai display time");
}

bool time_service_is_synced(void) {
    if (!s_synced && esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
        update_boot_epoch();
    }
    time_service_snapshot_t snapshot;
    return time_service_get_snapshot(&snapshot) && snapshot.synced;
}

bool time_service_get_snapshot(time_service_snapshot_t *out) {
    if (!out) {
        return false;
    }

    portENTER_CRITICAL(&s_time_lock);
    out->synced = s_synced;
    out->boot_epoch_ms = s_boot_epoch_ms;
    out->sync_count = s_sync_count;
    portEXIT_CRITICAL(&s_time_lock);
    out->now_epoch_ms = out->synced ? current_epoch_ms() : 0;
    return true;
}

bool time_service_uptime_to_epoch_ms(int64_t uptime_ms, int64_t *out_epoch_ms) {
    time_service_snapshot_t snapshot;
    if (!out_epoch_ms || !time_service_is_synced() ||
        !time_service_get_snapshot(&snapshot) || !snapshot.synced) {
        return false;
    }
    *out_epoch_ms = snapshot.boot_epoch_ms + uptime_ms;
    return true;
}

int64_t time_service_now_epoch_ms(void) {
    time_service_snapshot_t snapshot;
    return time_service_is_synced() && time_service_get_snapshot(&snapshot)
               ? snapshot.now_epoch_ms
               : 0;
}
