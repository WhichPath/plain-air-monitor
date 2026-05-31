#include "runtime_config.h"

#include "esp_log.h"
#include "nvs.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "runtime_config";

#define ML_CONFIG_NVS_NAMESPACE "ml_config"
#define ML_CONFIG_NVS_KEY_SETTINGS "settings"
#define ML_CONFIG_NVS_KEY_WIFI "wifi_list"
#define ML_CONFIG_MAX_WIFI_ENTRIES 16
#define ML_CONFIG_SETTINGS_VERSION_MIN 1

typedef struct __attribute__((packed)) {
    char ssid[33];
    char pass[65];
} runtime_wifi_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t count;
    uint8_t active_idx;
    runtime_wifi_entry_t entries[ML_CONFIG_MAX_WIFI_ENTRIES];
} runtime_wifi_list_t;

typedef struct __attribute__((packed)) {
    uint8_t version;
    char wifi_ssid[33];
    char wifi_pass[65];
    char auth_key[96];
    char device_prefix[32];
    char cellular_apn[32];
    char cellular_sim_pin[16];
    uint8_t flags;
    uint8_t max_peers;
    uint16_t disco_heartbeat_ms;
    uint32_t priority_peer_ip;
    char ctrl_host[64];
    uint8_t debug_flags;
    char device_name_full[48];
    char ppp_user[32];
    char ppp_pass[32];
} runtime_settings_t;

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

static bool load_wifi_list_from_nvs(wifi_station_config_t *config) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(ML_CONFIG_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    runtime_wifi_list_t *list = calloc(1, sizeof(*list));
    if (!list) {
        nvs_close(nvs);
        return false;
    }
    list->active_idx = 0xFF;

    size_t stored_len = 0;
    err = nvs_get_blob(nvs, ML_CONFIG_NVS_KEY_WIFI, NULL, &stored_len);
    if (err == ESP_OK && stored_len >= offsetof(runtime_wifi_list_t, entries) &&
        stored_len <= sizeof(*list)) {
        size_t read_len = stored_len;
        err = nvs_get_blob(nvs, ML_CONFIG_NVS_KEY_WIFI, list, &read_len);
        if (err == ESP_OK && list->count > 0 &&
            list->count <= ML_CONFIG_MAX_WIFI_ENTRIES &&
            read_len >= offsetof(runtime_wifi_list_t, entries) +
                            list->count * sizeof(list->entries[0])) {
            for (uint8_t i = 0; i < list->count; i++) {
                add_wifi_network(config, list->entries[i].ssid,
                                 list->entries[i].pass);
            }
            free(list);
            nvs_close(nvs);
            return config->network_count > 0;
        }
    }
    free(list);

    runtime_settings_t *settings = calloc(1, sizeof(*settings));
    if (!settings) {
        nvs_close(nvs);
        return false;
    }
    size_t settings_len = sizeof(*settings);
    err = nvs_get_blob(nvs, ML_CONFIG_NVS_KEY_SETTINGS, settings,
                       &settings_len);
    nvs_close(nvs);

    if (err == ESP_OK && settings_len >= ML_CONFIG_SETTINGS_VERSION_MIN &&
        settings->version >= ML_CONFIG_SETTINGS_VERSION_MIN &&
        settings->wifi_ssid[0] != '\0') {
        add_wifi_network(config, settings->wifi_ssid, settings->wifi_pass);
        free(settings);
        return config->network_count > 0;
    }

    free(settings);
    return false;
}

void runtime_config_load_wifi(wifi_station_config_t *config,
                              const char *fallback_ssid,
                              const char *fallback_password) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    if (load_wifi_list_from_nvs(config)) {
        ESP_LOGI(TAG, "Wi-Fi runtime config: %u networks",
                 (unsigned)config->network_count);
        return;
    }

    char ssid[33] = "";
    char password[65] = "";
    strlcpy(ssid, fallback_ssid ? fallback_ssid : "", sizeof(ssid));
    strlcpy(password, fallback_password ? fallback_password : "",
            sizeof(password));

    ESP_LOGI(TAG, "using build-time Wi-Fi: %s", ssid);
    add_wifi_network(config, ssid, password);
}
