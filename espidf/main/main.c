/**
 * @file main.c
 * @brief Firmware entry point for the MicroLink sensor dashboard.
 *
 * main only wires services together. Component-level code owns Wi-Fi,
 * tailnet, sensor sampling, storage, and HTTP presentation.
 */

#include "data_store.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "pm_credentials.h"
#include "runtime_config.h"
#include "sensor_service.h"
#include "tailnet_service.h"
#include "time_service.h"
#include "web_server.h"
#include "wifi_station.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

#define BOARD_POWER_ON_GPIO 15
#define OTA_VALIDATE_TIMEOUT_MS (3 * 60 * 1000)

static wifi_station_config_t wifi_config;
static char wifi_ip[16];

static void board_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_POWER_ON_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(BOARD_POWER_ON_GPIO, 1);
}

static const char *device_name(void) {
    return PM_DEVICE_NAME[0] ? PM_DEVICE_NAME : "microlink-sensor";
}

#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
static bool running_app_pending_verify(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    esp_err_t err = esp_ota_get_state_partition(running, &ota_state);
    return err == ESP_OK && ota_state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void ota_validate_watchdog_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_VALIDATE_TIMEOUT_MS));
    if (running_app_pending_verify()) {
        ESP_LOGE(TAG, "OTA app was not marked valid within %u ms; rebooting for rollback",
                 OTA_VALIDATE_TIMEOUT_MS);
        esp_restart();
    }
    vTaskDelete(NULL);
}
#endif

static void start_ota_validate_watchdog_if_pending(void) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (running_app_pending_verify()) {
        xTaskCreate(ota_validate_watchdog_task, "ota_validate", 2048, NULL, 5,
                    NULL);
    }
#endif
}

static void confirm_ota_app_if_pending(void) {
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    if (!running_app_pending_verify()) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA app marked valid after startup checks");
    } else {
        ESP_LOGE(TAG, "failed to mark OTA app valid: %s", esp_err_to_name(err));
    }
#endif
}

void app_main(void) {
    board_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    start_ota_validate_watchdog_if_pending();

    ESP_ERROR_CHECK(data_store_init());
    ESP_ERROR_CHECK(sensor_service_start(data_store_add_sample, NULL));

    ESP_LOGI(TAG, "MicroLink sensor dashboard firmware");
    ESP_LOGI(TAG, "free heap: %lu bytes (PSRAM: %lu bytes)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    runtime_config_load_wifi(&wifi_config, PM_WIFI_SSID, PM_WIFI_PASSWORD);
    ESP_ERROR_CHECK(station_start(&wifi_config));
    ESP_ERROR_CHECK(station_wait_connected(portMAX_DELAY));
    time_service_start();

    station_get_ip(wifi_ip);

    web_server_config_t web_config = {
        .device_name = device_name(),
    };
    ESP_ERROR_CHECK(web_server_start(&web_config));
    ESP_LOGI(TAG, "Wi-Fi dashboard: http://%s/", wifi_ip);

    tailnet_config_t tailnet_config = {
        .auth_key = PM_TAILSCALE_AUTH_KEY,
        .device_name = device_name(),
        .priority_peer_ip = CONFIG_ML_PRIORITY_PEER_IP,
        .diagnostic_target_ip = CONFIG_ML_EXAMPLE_TARGET_PEER_IP,
        .max_peers = CONFIG_ML_MAX_PEERS,
        .wifi_tx_power_dbm = 13,
    };
    ESP_ERROR_CHECK(tailnet_service_start(&tailnet_config));
    confirm_ota_app_if_pending();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        sensor_status_t sensor_status;
        sensor_service_get_status(&sensor_status);

        tailnet_status_t tailnet_status;
        tailnet_service_get_status(&tailnet_status);

        sensor_sample_t latest;
        bool has_latest = sensor_service_get_latest(&latest);

        ESP_LOGI(TAG, "status: tailnet=%s peers=%d sensor=%s reads=%lu errors=%lu sht45=%s sht45_reads=%lu temp=%.2fC humidity=%.2f%% heap=%lu",
                 tailnet_status.state,
                 tailnet_status.peer_count,
                 sensor_state_name(sensor_status.state),
                 (unsigned long)sensor_status.read_count,
                 (unsigned long)sensor_status.error_count,
                 sensor_state_name(sensor_status.sht45_state),
                 (unsigned long)sensor_status.sht45_read_count,
                 has_latest && latest.has_temperature ? latest.temperature_c : 0.0f,
                 has_latest && latest.has_humidity ? latest.humidity_percent : 0.0f,
                 (unsigned long)esp_get_free_heap_size());
    }
}
