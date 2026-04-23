#include "wifi_station.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi_station";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_MAX_RETRIES_PER_NETWORK 3

static EventGroupHandle_t wifi_event_group;
static wifi_station_config_t station_config;
static size_t current_network_idx;
static int retry_count;
static char wifi_ip_str[16] = "0.0.0.0";
static bool started;

static void connect_current_network(void) {
    if (station_config.network_count == 0) {
        ESP_LOGE(TAG, "no Wi-Fi networks configured");
        return;
    }

    const wifi_network_t *network = &station_config.networks[current_network_idx];
    wifi_config_t wifi_config = {
        .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK },
    };
    strncpy((char *)wifi_config.sta.ssid, network->ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, network->password,
            sizeof(wifi_config.sta.password) - 1);

    ESP_LOGI(TAG, "trying network %u/%u: %s",
             (unsigned)(current_network_idx + 1),
             (unsigned)station_config.network_count,
             network->ssid);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_connect();
}

static void try_next_network(void) {
    if (station_config.network_count <= 1) {
        esp_wifi_connect();
        return;
    }

    retry_count++;
    if (retry_count >= WIFI_MAX_RETRIES_PER_NETWORK) {
        retry_count = 0;
        current_network_idx = (current_network_idx + 1) % station_config.network_count;
    }
    connect_current_network();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        connect_current_network();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "disconnected, reason=%d", disc->reason);
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        try_next_network();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(wifi_ip_str, sizeof(wifi_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "connected to %s, IP: " IPSTR,
                 station_config.networks[current_network_idx].ssid,
                 IP2STR(&event->ip_info.ip));
        retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t station_start(const wifi_station_config_t *config) {
    if (started) {
        return ESP_OK;
    }
    if (!config || config->network_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&station_config, 0, sizeof(station_config));
    station_config.network_count = config->network_count;
    if (station_config.network_count > WIFI_STATION_MAX_NETWORKS) {
        station_config.network_count = WIFI_STATION_MAX_NETWORKS;
    }
    for (size_t i = 0; i < station_config.network_count; i++) {
        station_config.networks[i] = config->networks[i];
    }

    wifi_event_group = xEventGroupCreate();
    if (!wifi_event_group) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    started = true;
    ESP_LOGI(TAG, "Wi-Fi station started");
    return ESP_OK;
}

esp_err_t station_wait_connected(TickType_t timeout_ticks) {
    if (!wifi_event_group) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, timeout_ticks);
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

void station_get_ip(char out[16]) {
    if (!out) {
        return;
    }
    strlcpy(out, wifi_ip_str, 16);
}

bool station_get_rssi(int *out_rssi) {
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    if (out_rssi) {
        *out_rssi = ap.rssi;
    }
    return true;
}
