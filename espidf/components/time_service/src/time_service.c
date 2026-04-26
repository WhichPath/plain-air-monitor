#include "time_service.h"

#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

static const char *TAG = "time_service";

static bool s_started;
static volatile bool s_synced;
static int64_t s_boot_epoch_ms;

static int64_t current_epoch_ms(void) {
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    return ((int64_t)tv.tv_sec * 1000LL) + ((int64_t)tv.tv_usec / 1000LL);
}

static void update_boot_epoch(void) {
    int64_t uptime_ms = esp_timer_get_time() / 1000LL;
    int64_t epoch_ms = current_epoch_ms();
    s_boot_epoch_ms = epoch_ms - uptime_ms;
    s_synced = true;
}

static void sync_cb(struct timeval *tv) {
    (void)tv;
    update_boot_epoch();
    ESP_LOGI(TAG, "time synced: boot_epoch_ms=%lld now_epoch_ms=%lld",
             (long long)s_boot_epoch_ms,
             (long long)current_epoch_ms());
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
    return s_synced;
}

bool time_service_uptime_to_epoch_ms(int64_t uptime_ms, int64_t *out_epoch_ms) {
    if (!out_epoch_ms || !time_service_is_synced()) {
        return false;
    }
    *out_epoch_ms = s_boot_epoch_ms + uptime_ms;
    return true;
}

int64_t time_service_now_epoch_ms(void) {
    return time_service_is_synced() ? current_epoch_ms() : 0;
}
