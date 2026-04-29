#include "runtime_config.h"

#include "esp_log.h"
#include "ml_config_httpd.h"
#include <string.h>

static const char *TAG = "runtime_config";
static ml_config_wifi_list_t s_wifi_list;

static void add_wifi_network(wifi_station_config_t *config,
                             const char *ssid,
                             const char *password) {
    if (!config || !ssid || !ssid[0] ||
        config->network_count >= WIFI_STATION_MAX_NETWORKS) {
        return;
    }

    wifi_network_t *network = &config->networks[config->network_count++];
    strlcpy(network->ssid, ssid, sizeof(network->ssid));
    strlcpy(network->password, password ? password : "",
            sizeof(network->password));
}

void runtime_config_load_wifi(wifi_station_config_t *config,
                              const char *fallback_ssid,
                              const char *fallback_password) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    memset(&s_wifi_list, 0, sizeof(s_wifi_list));
    s_wifi_list.active_idx = 0xFF;
    if (ml_config_get_wifi_list(&s_wifi_list) && s_wifi_list.count > 1) {
        for (uint8_t i = 0; i < s_wifi_list.count; i++) {
            add_wifi_network(config, s_wifi_list.entries[i].ssid,
                             s_wifi_list.entries[i].pass);
        }
        ESP_LOGI(TAG, "Wi-Fi runtime config: %u networks",
                 (unsigned)config->network_count);
        return;
    }

    char ssid[33] = "";
    char password[65] = "";
    strlcpy(ssid, fallback_ssid ? fallback_ssid : "", sizeof(ssid));
    strlcpy(password, fallback_password ? fallback_password : "",
            sizeof(password));

    if (ml_config_get_nvs_wifi(ssid, sizeof(ssid), password,
                               sizeof(password))) {
        ESP_LOGI(TAG, "using NVS Wi-Fi: %s", ssid);
    } else {
        ESP_LOGI(TAG, "using build-time Wi-Fi: %s", ssid);
    }
    add_wifi_network(config, ssid, password);
}
