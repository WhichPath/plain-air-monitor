#include "tailnet_service.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "microlink.h"
#include "wifi_station.h"
#include <string.h>

static const char *TAG = "tailnet_service";

#define DIAGNOSTIC_PORT 9000
#define DIAGNOSTIC_SEND_INTERVAL_MS 5000
#define TAILNET_HEALTH_INTERVAL_MS 10000
#define TAILNET_REBIND_COOLDOWN_MS 120000
#define TAILNET_NOT_CONNECTED_REBIND_MS 180000
#define TAILNET_DERP_STALE_MS 300000

static microlink_t *ml;
static microlink_udp_socket_t *udp_sock;
static SemaphoreHandle_t tailnet_mutex;
static TaskHandle_t peer_warm_task_handle;
static TaskHandle_t diagnostic_task_handle;
static TaskHandle_t health_task_handle;
static uint32_t diagnostic_target_ip;
static uint32_t msg_tx_count;
static uint32_t msg_rx_count;
static uint64_t last_rebind_ms;
static uint64_t not_connected_since_ms;
static uint64_t derp_down_since_ms;
static char last_wifi_ip[16];

static const char *state_name(microlink_state_t state) {
    switch (state) {
        case ML_STATE_IDLE: return "idle";
        case ML_STATE_WIFI_WAIT: return "wifi_wait";
        case ML_STATE_CONNECTING: return "connecting";
        case ML_STATE_REGISTERING: return "registering";
        case ML_STATE_CONNECTED: return "connected";
        case ML_STATE_RECONNECTING: return "reconnecting";
        case ML_STATE_ERROR: return "error";
        default: return "unknown";
    }
}

static void on_udp_rx(microlink_udp_socket_t *sock, uint32_t src_ip, uint16_t src_port,
                      const uint8_t *data, size_t len, void *user_data) {
    (void)user_data;
    msg_rx_count++;

    char ip_str[16];
    microlink_ip_to_str(src_ip, ip_str);

    char msg[256];
    size_t copy_len = (len < sizeof(msg) - 1) ? len : sizeof(msg) - 1;
    memcpy(msg, data, copy_len);
    msg[copy_len] = '\0';
    if (copy_len > 0 && msg[copy_len - 1] == '\n') {
        msg[copy_len - 1] = '\0';
    }

    ESP_LOGI(TAG, "UDP RX #%lu from %s:%u [%d bytes]: \"%s\"",
             (unsigned long)msg_rx_count, ip_str, src_port, (int)len, msg);

    char reply[300];
    int reply_len = snprintf(reply, sizeof(reply), "ECHO: %s", msg);
    if (reply_len > 0) {
        esp_err_t err = microlink_udp_send(sock, src_ip, src_port, reply, reply_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "UDP echo failed: %d", err);
        }
    }
}

static void ensure_udp_socket(void) {
    if (!tailnet_mutex ||
        xSemaphoreTake(tailnet_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return;
    }

    if (!ml || udp_sock || !microlink_is_connected(ml)) {
        xSemaphoreGive(tailnet_mutex);
        return;
    }

    udp_sock = microlink_udp_create(ml, DIAGNOSTIC_PORT);
    if (!udp_sock) {
        ESP_LOGE(TAG, "failed to create UDP diagnostic socket");
        xSemaphoreGive(tailnet_mutex);
        return;
    }

    microlink_udp_set_rx_callback(udp_sock, on_udp_rx, NULL);
    xSemaphoreGive(tailnet_mutex);
}

static void close_udp_socket_locked(void) {
    if (udp_sock) {
        microlink_udp_close(udp_sock);
        udp_sock = NULL;
    }
}

static bool request_rebind(const char *reason, uint64_t now_ms) {
    if (!ml || !station_is_connected()) {
        return false;
    }
    if (last_rebind_ms != 0 && now_ms - last_rebind_ms < TAILNET_REBIND_COOLDOWN_MS) {
        return false;
    }

    if (tailnet_mutex &&
        xSemaphoreTake(tailnet_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }

    ESP_LOGW(TAG, "tailnet rebind requested: %s", reason);
    last_rebind_ms = now_ms;
    close_udp_socket_locked();
    esp_err_t err = microlink_rebind(ml);
    if (err == ESP_OK) {
        not_connected_since_ms = 0;
        derp_down_since_ms = 0;
    } else {
        ESP_LOGW(TAG, "tailnet rebind failed: %s", esp_err_to_name(err));
    }

    if (tailnet_mutex) {
        xSemaphoreGive(tailnet_mutex);
    }
    return err == ESP_OK;
}

static void peer_warm_task(void *arg) {
    (void)arg;

    while (1) {
        if (ml && microlink_is_connected(ml)) {
            int peer_count = microlink_get_peer_count(ml);
            for (int i = 0; i < peer_count; i++) {
                microlink_peer_info_t info;
                if (microlink_get_peer_info(ml, i, &info) == ESP_OK && info.vpn_ip != 0) {
                    (void)microlink_warm_peer(ml, info.vpn_ip);
                    vTaskDelay(pdMS_TO_TICKS(200));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void diagnostic_task(void *arg) {
    (void)arg;
    uint64_t last_send_ms = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ensure_udp_socket();

        uint64_t now = (uint64_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (udp_sock && diagnostic_target_ip != 0 &&
            now - last_send_ms >= DIAGNOSTIC_SEND_INTERVAL_MS) {
            if (tailnet_mutex &&
                xSemaphoreTake(tailnet_mutex, pdMS_TO_TICKS(250)) == pdTRUE) {
                if (udp_sock) {
                    last_send_ms = now;
                    msg_tx_count++;
                    char msg[128];
                    int msg_len = snprintf(msg, sizeof(msg), "hello from ESP32 #%lu",
                                           (unsigned long)msg_tx_count);
                    microlink_udp_send(udp_sock, diagnostic_target_ip, DIAGNOSTIC_PORT,
                                       msg, msg_len);
                }
                xSemaphoreGive(tailnet_mutex);
            }
        }
    }
}

static void health_task(void *arg) {
    (void)arg;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TAILNET_HEALTH_INTERVAL_MS));
        if (!ml || !station_is_connected()) {
            not_connected_since_ms = 0;
            derp_down_since_ms = 0;
            continue;
        }

        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);

        char wifi_ip[16];
        station_get_ip(wifi_ip);
        if (wifi_ip[0] != '\0' && strcmp(wifi_ip, "0.0.0.0") != 0) {
            if (last_wifi_ip[0] != '\0' && strcmp(last_wifi_ip, wifi_ip) != 0) {
                strlcpy(last_wifi_ip, wifi_ip, sizeof(last_wifi_ip));
                request_rebind("wifi IP changed", now_ms);
                continue;
            }
            strlcpy(last_wifi_ip, wifi_ip, sizeof(last_wifi_ip));
        }

        microlink_state_t state = microlink_get_state(ml);
        if (state != ML_STATE_CONNECTED) {
            if (not_connected_since_ms == 0) {
                not_connected_since_ms = now_ms;
            } else if (now_ms - not_connected_since_ms >= TAILNET_NOT_CONNECTED_REBIND_MS) {
                request_rebind("tailnet not connected", now_ms);
            }
            continue;
        }
        not_connected_since_ms = 0;

        microlink_transport_status_t transport = {0};
        (void)microlink_get_transport_status(ml, &transport);
        if (!transport.derp_connected) {
            if (derp_down_since_ms == 0) {
                derp_down_since_ms = now_ms;
            } else if (now_ms - derp_down_since_ms >= TAILNET_NOT_CONNECTED_REBIND_MS) {
                request_rebind("DERP disconnected", now_ms);
            }
            continue;
        }
        derp_down_since_ms = 0;

        if (transport.derp_last_recv_ms != 0 &&
            now_ms - transport.derp_last_recv_ms >= TAILNET_DERP_STALE_MS) {
            request_rebind("DERP receive watchdog stale", now_ms);
        }
    }
}

static void on_state_change(microlink_t *ml_handle, microlink_state_t state, void *user_data) {
    (void)user_data;
    ESP_LOGI(TAG, "state: %s", state_name(state));

    if (state == ML_STATE_CONNECTED) {
        uint32_t ip = microlink_get_vpn_ip(ml_handle);
        char ip_str[16];
        microlink_ip_to_str(ip, ip_str);
        ESP_LOGI(TAG, "connected. Dashboard: http://%s/ UDP diagnostics: %s:%d",
                 ip_str, ip_str, DIAGNOSTIC_PORT);
        ensure_udp_socket();
        if (!peer_warm_task_handle) {
            xTaskCreatePinnedToCore(peer_warm_task, "peer_warm", 4096, NULL, 4,
                                    &peer_warm_task_handle, 1);
        }
    }
}

static void on_peer_update(microlink_t *ml_handle, const microlink_peer_info_t *peer,
                           void *user_data) {
    (void)ml_handle;
    (void)user_data;
    char ip_str[16];
    microlink_ip_to_str(peer->vpn_ip, ip_str);
    ESP_LOGI(TAG, "peer: %s (%s) online=%d direct=%d",
             peer->hostname, ip_str, peer->online, peer->direct_path);
}

esp_err_t tailnet_service_start(const tailnet_config_t *config) {
    if (ml) {
        return ESP_OK;
    }
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!tailnet_mutex) {
        tailnet_mutex = xSemaphoreCreateMutex();
        if (!tailnet_mutex) {
            return ESP_ERR_NO_MEM;
        }
    }

    diagnostic_target_ip = 0;
    if (config->diagnostic_target_ip && config->diagnostic_target_ip[0] != '\0') {
        diagnostic_target_ip = microlink_parse_ip(config->diagnostic_target_ip);
    }

    microlink_config_t ml_config = {
        .auth_key = config->auth_key,
        .device_name = config->device_name,
        .enable_derp = true,
        .enable_stun = true,
        .enable_disco = true,
        .max_peers = config->max_peers,
        .wifi_tx_power_dbm = config->wifi_tx_power_dbm,
        .priority_peer_ip = microlink_parse_ip(config->priority_peer_ip),
    };

    ml = microlink_init(&ml_config);
    if (!ml) {
        ESP_LOGE(TAG, "failed to initialize MicroLink");
        return ESP_FAIL;
    }

    microlink_set_state_callback(ml, on_state_change, NULL);
    microlink_set_peer_callback(ml, on_peer_update, NULL);
    esp_err_t err = microlink_start(ml);
    if (err != ESP_OK) {
        return err;
    }

    if (!diagnostic_task_handle) {
        xTaskCreatePinnedToCore(diagnostic_task, "tailnet_diag", 4096, NULL, 4,
                                &diagnostic_task_handle, 1);
    }
    if (!health_task_handle) {
        xTaskCreatePinnedToCore(health_task, "tailnet_health", 4096, NULL, 4,
                                &health_task_handle, 1);
    }
    return ESP_OK;
}

void tailnet_service_get_status(tailnet_status_t *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    strlcpy(out->vpn_ip, "", sizeof(out->vpn_ip));
    strlcpy(out->state, ml ? state_name(microlink_get_state(ml)) : "offline",
            sizeof(out->state));
    out->peer_count = ml ? microlink_get_peer_count(ml) : 0;
    out->connected = ml ? microlink_is_connected(ml) : false;

    uint32_t ip = ml ? microlink_get_vpn_ip(ml) : 0;
    if (ip) {
        microlink_ip_to_str(ip, out->vpn_ip);
    }
}
