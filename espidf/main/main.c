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
#include "esp_system.h"
#include "nvs_flash.h"
#include "pm_credentials.h"
#include "sensor_service.h"
#include "tailnet_service.h"
#include "web_server.h"
#include "wifi_station.h"

#include "ml_config_httpd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "app";

#define BOARD_POWER_ON_GPIO 15

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

static void add_wifi_network(wifi_station_config_t *config,
                             const char *ssid,
                             const char *password) {
    if (!config || !ssid || !ssid[0] ||
        config->network_count >= WIFI_STATION_MAX_NETWORKS) {
        return;
    }

    wifi_network_t *network = &config->networks[config->network_count++];
    strlcpy(network->ssid, ssid, sizeof(network->ssid));
    strlcpy(network->password, password ? password : "", sizeof(network->password));
}

static void load_wifi_config(wifi_station_config_t *config) {
    memset(config, 0, sizeof(*config));

    ml_config_wifi_list_t wifi_list = {0};
    wifi_list.active_idx = 0xFF;
    if (ml_config_get_wifi_list(&wifi_list) && wifi_list.count > 1) {
        for (uint8_t i = 0; i < wifi_list.count; i++) {
            add_wifi_network(config, wifi_list.entries[i].ssid, wifi_list.entries[i].pass);
        }
        ESP_LOGI(TAG, "Wi-Fi multi-SSID config: %u networks", (unsigned)config->network_count);
        return;
    }

    char ssid[33] = PM_WIFI_SSID;
    char password[65] = PM_WIFI_PASSWORD;
    if (ml_config_get_nvs_wifi(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "using NVS Wi-Fi: %s", ssid);
    } else {
        ESP_LOGI(TAG, "using build-time Wi-Fi: %s", ssid);
    }
    add_wifi_network(config, ssid, password);
}

static const char *device_name(void) {
    return PM_DEVICE_NAME[0] ? PM_DEVICE_NAME : "microlink-sensor";
}

void app_main(void) {
    board_init();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(data_store_init());
    ESP_ERROR_CHECK(sensor_service_start(data_store_add_sample, NULL));

    ESP_LOGI(TAG, "MicroLink sensor dashboard firmware");
    ESP_LOGI(TAG, "free heap: %lu bytes (PSRAM: %lu bytes)",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    wifi_station_config_t wifi_config;
    load_wifi_config(&wifi_config);
    ESP_ERROR_CHECK(station_start(&wifi_config));
    ESP_ERROR_CHECK(station_wait_connected(portMAX_DELAY));

    char wifi_ip[16];
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

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));

        sensor_status_t sensor_status;
        sensor_service_get_status(&sensor_status);

        tailnet_status_t tailnet_status;
        tailnet_service_get_status(&tailnet_status);

        ESP_LOGI(TAG, "status: tailnet=%s peers=%d sensor=%s reads=%lu errors=%lu heap=%lu",
                 tailnet_status.state,
                 tailnet_status.peer_count,
                 sensor_state_name(sensor_status.state),
                 (unsigned long)sensor_status.read_count,
                 (unsigned long)sensor_status.error_count,
                 (unsigned long)esp_get_free_heap_size());
    }
}
