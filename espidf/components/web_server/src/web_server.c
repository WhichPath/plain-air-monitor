#include "web_server.h"

#include "data_store.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sensor_service.h"
#include "tailnet_service.h"
#include "time_service.h"
#include "wifi_station.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";
static httpd_handle_t server;
static char device_name[48] = "microlink-sensor";

extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[] asm("_binary_dashboard_html_end");

static esp_err_t handler_root(httpd_req_t *req) {
    size_t len = dashboard_html_end - dashboard_html_start;
    if (len > 0 && dashboard_html_start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)dashboard_html_start, len);
}

static esp_err_t handler_metrics(httpd_req_t *req) {
    tailnet_status_t tailnet_status;
    tailnet_service_get_status(&tailnet_status);

    int wifi_rssi = 0;
    bool has_rssi = station_get_rssi(&wifi_rssi);
    char wifi_ip[16];
    station_get_ip(wifi_ip);
    char rssi_json[16];
    if (has_rssi) {
        snprintf(rssi_json, sizeof(rssi_json), "%d", wifi_rssi);
    } else {
        snprintf(rssi_json, sizeof(rssi_json), "null");
    }

    sensor_sample_t sample = {0};
    bool has_sample = sensor_service_get_latest(&sample);

    sensor_status_t sensor_status;
    sensor_service_get_status(&sensor_status);

    uint32_t stored_records = 0;
    uint32_t active_samples = 0;
    float active_pm25 = 0.0f;
    data_store_get_summary(&stored_records, &active_samples, &active_pm25);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    int n = snprintf(buf, 2048,
        "{\"device\":\"%s\",\"wifi_ip\":\"%s\",\"vpn_ip\":\"%s\",\"ml_state\":\"%s\","
        "\"peer_count\":%d,\"uptime_ms\":%lld,\"heap_free\":%lu,"
        "\"time_synced\":%s,\"epoch_ms\":%lld,"
        "\"psram_free\":%lu,\"wifi_rssi\":%s,"
        "\"sensor\":{\"state\":\"%s\",\"last_error\":%d,\"error_count\":%lu,"
        "\"read_count\":%lu,\"app_read_failures\":%lu},"
        "\"sht45\":{\"state\":\"%s\",\"detected\":%s,\"last_error\":%d,"
        "\"error_count\":%lu,\"read_count\":%lu,\"serial\":%lu},"
        "\"data\":{\"stored\":%lu,\"active_samples\":%lu,\"active_pm25\":%.2f},"
        "\"latest\":",
        device_name,
        wifi_ip,
        tailnet_status.vpn_ip,
        tailnet_status.state,
        tailnet_status.peer_count,
        (long long)(esp_timer_get_time() / 1000),
        (unsigned long)esp_get_free_heap_size(),
        time_service_is_synced() ? "true" : "false",
        (long long)time_service_now_epoch_ms(),
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        rssi_json,
        sensor_state_name(sensor_status.state),
        sensor_status.last_error,
        (unsigned long)sensor_status.error_count,
        (unsigned long)sensor_status.read_count,
        (unsigned long)sensor_status.app_read_failures,
        sensor_state_name(sensor_status.sht45_state),
        sensor_status.sht45_detected ? "true" : "false",
        sensor_status.sht45_last_error,
        (unsigned long)sensor_status.sht45_error_count,
        (unsigned long)sensor_status.sht45_read_count,
        (unsigned long)sensor_status.sht45_serial,
        (unsigned long)stored_records,
        (unsigned long)active_samples,
        active_pm25);

    if (n < 0 || n >= 2048) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, buf, n);

    if (has_sample) {
        n = snprintf(buf, 2048,
            "{\"pm1_0\":%.2f,\"pm2_5\":%.2f,\"pm4_0\":%.2f,\"pm10_0\":%.2f,"
            "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,"
            "\"nc10_0\":%.2f,\"typical_particle_size\":%.3f,"
            "\"temperature_c\":%.2f,\"humidity_percent\":%.2f,"
            "\"has_temperature\":%s,\"has_humidity\":%s,"
            "\"timestamp_ms\":%lld}}",
            sample.pm1_0, sample.pm2_5, sample.pm4_0, sample.pm10_0,
            sample.nc0_5, sample.nc1_0, sample.nc2_5, sample.nc4_0,
            sample.nc10_0, sample.typical_particle_size,
            sample.temperature_c,
            sample.humidity_percent,
            sample.has_temperature ? "true" : "false",
            sample.has_humidity ? "true" : "false",
            (long long)sample.timestamp_ms);
    } else {
        n = snprintf(buf, 2048, "null}");
    }

    if (n < 0 || n >= 2048) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, buf, n);
    esp_err_t err = httpd_resp_send_chunk(req, NULL, 0);
    free(buf);
    return err;
}

static esp_err_t handler_history(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "{\"interval_ms\":2000,\"samples\":[");

    uint16_t count = sensor_service_get_history_count();
    for (uint16_t i = 0; i < count; i++) {
        sensor_history_point_t point;
        if (!sensor_service_get_history_point(i, &point)) {
            continue;
        }

        char item[260];
        int n = snprintf(item, sizeof(item),
                         "%s{\"t\":%lld,\"pm1\":%.2f,\"pm25\":%.2f,\"pm10\":%.2f,"
                         "\"temperature_c\":%.2f,\"humidity_percent\":%.2f,"
                         "\"has_temperature\":%s,\"has_humidity\":%s}",
                         i ? "," : "",
                         (long long)point.timestamp_ms,
                         point.pm1_0,
                         point.pm2_5,
                         point.pm10_0,
                         point.temperature_c,
                         point.humidity_percent,
                         point.has_temperature ? "true" : "false",
                         point.has_humidity ? "true" : "false");
        if (n > 0) {
            httpd_resp_send_chunk(req, item, n);
        }
    }

    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

#define DATA_API_MAX_RECORDS 1008u

static uint32_t data_api_record_limit(httpd_req_t *req) {
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return DATA_API_MAX_RECORDS;
    }

    char limit_str[16];
    if (httpd_query_key_value(query, "limit", limit_str, sizeof(limit_str)) != ESP_OK) {
        return DATA_API_MAX_RECORDS;
    }

    char *end = NULL;
    unsigned long value = strtoul(limit_str, &end, 10);
    if (end == limit_str || *end != '\0' || value == 0) {
        return DATA_API_MAX_RECORDS;
    }
    if (value > DATA_API_MAX_RECORDS) {
        return DATA_API_MAX_RECORDS;
    }
    return (uint32_t)value;
}

static esp_err_t handler_data(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    if (!data_store_lock(pdMS_TO_TICKS(500))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "data store busy");
        return ESP_FAIL;
    }

    data_active_t active;
    data_store_get_active_locked(&active);
    uint32_t stored = data_store_record_count_locked();
    uint32_t capacity = data_store_record_capacity_locked();
    uint32_t limit = data_api_record_limit(req);
    uint32_t start = (stored > limit) ? stored - limit : 0;
    uint32_t returned = stored - start;

    char head[768];
    int n = snprintf(head, sizeof(head),
                     "{\"window_ms\":%lld,\"stored\":%lu,\"capacity\":%lu,"
                     "\"returned\":%lu,"
                     "\"active\":{\"start_ms\":%lld,\"elapsed_ms\":%lu,"
                     "\"start_epoch_ms\":%lld,"
                     "\"sample_count\":%u,\"field_mask\":%u,"
                     "\"pm2_5_avg\":%.2f,\"pm2_5_min\":%.2f,\"pm2_5_max\":%.2f,"
                     "\"pm10_0_avg\":%.2f,\"pm10_0_min\":%.2f,\"pm10_0_max\":%.2f,"
                     "\"temperature_avg\":%.2f,\"temperature_min\":%.2f,\"temperature_max\":%.2f,"
                     "\"humidity_avg\":%.2f,\"humidity_min\":%.2f,\"humidity_max\":%.2f},"
                     "\"records\":[",
                     (long long)DATA_RECORD_WINDOW_MS,
                     (unsigned long)stored,
                     (unsigned long)capacity,
                     (unsigned long)returned,
                     (long long)active.start_ms,
                     (unsigned long)active.elapsed_ms,
                     (long long)active.start_epoch_ms,
                     active.sample_count,
                     active.field_mask,
                     active.pm2_5_avg,
                     active.pm2_5_min,
                     active.pm2_5_max,
                     active.pm10_0_avg,
                     active.pm10_0_min,
                     active.pm10_0_max,
                     active.temperature_avg,
                     active.temperature_min,
                     active.temperature_max,
                     active.humidity_avg,
                     active.humidity_min,
                     active.humidity_max);
    httpd_resp_send_chunk(req, head, n);

    uint32_t emitted = 0;
    for (uint32_t i = 0; i < returned; i++) {
        data_record_t record;
        if (!data_store_get_record_locked(start + i, &record)) {
            continue;
        }

        char item[640];
        n = snprintf(item, sizeof(item),
                     "%s{\"seq\":%lu,\"start_ms\":%lld,\"end_ms\":%lld,"
                     "\"start_epoch_ms\":%lld,\"end_epoch_ms\":%lld,"
                     "\"duration_ms\":%lu,\"sample_count\":%u,\"field_mask\":%u,"
                     "\"pm2_5_avg\":%.2f,\"pm2_5_min\":%.2f,\"pm2_5_max\":%.2f,"
                     "\"pm10_0_avg\":%.2f,\"pm10_0_min\":%.2f,\"pm10_0_max\":%.2f,"
                     "\"temperature_avg\":%.2f,\"temperature_min\":%.2f,\"temperature_max\":%.2f,"
                     "\"humidity_avg\":%.2f,\"humidity_min\":%.2f,\"humidity_max\":%.2f}",
                     emitted ? "," : "",
                     (unsigned long)record.seq,
                     (long long)record.start_ms,
                     (long long)record.end_ms,
                     (long long)record.start_epoch_ms,
                     (long long)record.end_epoch_ms,
                     (unsigned long)record.duration_ms,
                     record.sample_count,
                     record.field_mask,
                     record.pm2_5_avg,
                     record.pm2_5_min,
                     record.pm2_5_max,
                     record.pm10_0_avg,
                     record.pm10_0_min,
                     record.pm10_0_max,
                     record.temperature_avg,
                     record.temperature_min,
                     record.temperature_max,
                     record.humidity_avg,
                     record.humidity_min,
                     record.humidity_max);
        if (n > 0) {
            httpd_resp_send_chunk(req, item, n);
            emitted++;
        }
    }

    data_store_unlock();
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t web_server_start(const web_server_config_t *config) {
    if (server) {
        return ESP_OK;
    }

    if (config && config->device_name && config->device_name[0]) {
        strlcpy(device_name, config->device_name, sizeof(device_name));
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = 80;
    httpd_config.ctrl_port = 32768;
    httpd_config.stack_size = 12288;
    httpd_config.max_uri_handlers = 5;
    httpd_config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handler_root,
        .user_ctx = NULL,
    };
    const httpd_uri_t metrics = {
        .uri = "/api/metrics",
        .method = HTTP_GET,
        .handler = handler_metrics,
        .user_ctx = NULL,
    };
    const httpd_uri_t hist = {
        .uri = "/api/history",
        .method = HTTP_GET,
        .handler = handler_history,
        .user_ctx = NULL,
    };
    const httpd_uri_t data = {
        .uri = "/api/data",
        .method = HTTP_GET,
        .handler = handler_data,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &metrics);
    httpd_register_uri_handler(server, &hist);
    httpd_register_uri_handler(server, &data);

    ESP_LOGI(TAG, "HTTP dashboard listening on port 80");
    return ESP_OK;
}
