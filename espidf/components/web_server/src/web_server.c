#include "web_server.h"

#include "data_store.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sensor_service.h"
#include "tailnet_service.h"
#include "time_service.h"
#include "wifi_station.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "web_server";
static httpd_handle_t server;
static char device_name[48] = "microlink-sensor";

extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[] asm("_binary_dashboard_html_end");
extern const uint8_t sensirion_logo_png_start[] asm("_binary_sensirion_logo_png_start");
extern const uint8_t sensirion_logo_png_end[] asm("_binary_sensirion_logo_png_end");
extern const uint8_t bosch_logo_svg_start[] asm("_binary_bosch_logo_svg_start");
extern const uint8_t bosch_logo_svg_end[] asm("_binary_bosch_logo_svg_end");

#define MS_PER_DAY (24LL * 60LL * 60LL * 1000LL)
#define OTA_RECV_BUF_SIZE 4096

static const char *time_quality_name(bool verified, bool reconciled) {
    if (!verified) {
        return "unsynced_uptime";
    }
    return reconciled ? "reconciled_from_uptime" : "synced_epoch";
}

static void json_escape_copy(char *out, size_t out_size, const char *in) {
    if (!out || out_size == 0) {
        return;
    }
    if (!in) {
        in = "";
    }

    size_t pos = 0;
    for (; *in && pos + 1 < out_size; in++) {
        if ((*in == '"' || *in == '\\') && pos + 2 < out_size) {
            out[pos++] = '\\';
            out[pos++] = *in;
        } else if ((unsigned char)*in >= 0x20) {
            out[pos++] = *in;
        }
    }
    out[pos] = '\0';
}

static int format_record_json(char *buf, size_t size, const char *prefix,
                              const data_record_t *record);

static void reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t handler_root(httpd_req_t *req) {
    size_t len = dashboard_html_end - dashboard_html_start;
    if (len > 0 && dashboard_html_start[len - 1] == '\0') {
        len--;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)dashboard_html_start, len);
}

static esp_err_t send_asset(httpd_req_t *req, const uint8_t *start, const uint8_t *end, const char *type) {
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, (const char *)start, end - start);
}

static esp_err_t handler_sensirion_logo(httpd_req_t *req) {
    return send_asset(req, sensirion_logo_png_start, sensirion_logo_png_end, "image/png");
}

static esp_err_t handler_bosch_logo(httpd_req_t *req) {
    return send_asset(req, bosch_logo_svg_start, bosch_logo_svg_end, "image/svg+xml");
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

    char device_json[96];
    char wifi_ip_json[32];
    char vpn_ip_json[32];
    char tailnet_state_json[32];
    json_escape_copy(device_json, sizeof(device_json), device_name);
    json_escape_copy(wifi_ip_json, sizeof(wifi_ip_json), wifi_ip);
    json_escape_copy(vpn_ip_json, sizeof(vpn_ip_json), tailnet_status.vpn_ip);
    json_escape_copy(tailnet_state_json, sizeof(tailnet_state_json),
                     tailnet_status.state);

    uint32_t stored_records = 0;
    uint32_t active_samples = 0;
    float active_pm25 = 0.0f;
    data_store_get_summary(&stored_records, &active_samples, &active_pm25);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char *buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    int n = snprintf(buf, 4096,
        "{\"device\":\"%s\",\"wifi_ip\":\"%s\",\"vpn_ip\":\"%s\",\"ml_state\":\"%s\","
        "\"peer_count\":%d,\"uptime_ms\":%lld,\"heap_free\":%lu,"
        "\"time_synced\":%s,\"epoch_ms\":%lld,"
        "\"psram_free\":%lu,\"wifi_rssi\":%s,"
        "\"sensor\":{\"state\":\"%s\",\"last_error\":%d,\"error_count\":%lu,"
        "\"read_count\":%lu,\"app_read_failures\":%lu},"
        "\"sht45\":{\"state\":\"%s\",\"detected\":%s,\"last_error\":%d,"
        "\"error_count\":%lu,\"read_count\":%lu,\"serial\":%lu},"
        "\"bmp581\":{\"state\":\"%s\",\"detected\":%s,\"last_error\":%d,"
        "\"error_count\":%lu,\"read_count\":%lu,\"address\":%u,\"chip_id\":%u},"
        "\"scd41\":{\"state\":\"%s\",\"detected\":%s,\"last_error\":%d,"
        "\"error_count\":%lu,\"read_count\":%lu},"
        "\"sgp41\":{\"state\":\"%s\",\"detected\":%s,\"last_error\":%d,"
        "\"error_count\":%lu,\"read_count\":%lu,\"conditioning_remaining_s\":%u},"
        "\"data\":{\"stored\":%lu,\"active_frames\":%lu,\"active_pm25\":%.2f,"
        "\"history_days\":%.2f},"
        "\"latest\":",
        device_json,
        wifi_ip_json,
        vpn_ip_json,
        tailnet_state_json,
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
        sensor_state_name(sensor_status.bmp581_state),
        sensor_status.bmp581_detected ? "true" : "false",
        sensor_status.bmp581_last_error,
        (unsigned long)sensor_status.bmp581_error_count,
        (unsigned long)sensor_status.bmp581_read_count,
        sensor_status.bmp581_address,
        sensor_status.bmp581_chip_id,
        sensor_state_name(sensor_status.scd41_state),
        sensor_status.scd41_detected ? "true" : "false",
        sensor_status.scd41_last_error,
        (unsigned long)sensor_status.scd41_error_count,
        (unsigned long)sensor_status.scd41_read_count,
        sensor_state_name(sensor_status.sgp41_state),
        sensor_status.sgp41_detected ? "true" : "false",
        sensor_status.sgp41_last_error,
        (unsigned long)sensor_status.sgp41_error_count,
        (unsigned long)sensor_status.sgp41_read_count,
        sensor_status.sgp41_conditioning_remaining_s,
        (unsigned long)stored_records,
        (unsigned long)active_samples,
        active_pm25,
        (double)((int64_t)stored_records * DATA_RECORD_WINDOW_MS) / (double)MS_PER_DAY);

    if (n < 0 || n >= 4096) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_send_chunk(req, buf, n);

    if (has_sample) {
        n = snprintf(buf, 4096,
            "{\"pm1_0\":%.2f,\"pm2_5\":%.2f,\"pm4_0\":%.2f,\"pm10_0\":%.2f,"
            "\"nc0_5\":%.2f,\"nc1_0\":%.2f,\"nc2_5\":%.2f,\"nc4_0\":%.2f,"
            "\"nc10_0\":%.2f,\"typical_particle_size\":%.3f,"
            "\"temperature_c\":%.2f,\"humidity_percent\":%.2f,"
            "\"pressure_pa\":%.2f,\"co2_ppm\":%u,"
            "\"voc_index\":%.2f,\"nox_index\":%.2f,"
            "\"has_temperature\":%s,\"has_humidity\":%s,"
            "\"has_pressure\":%s,\"has_co2\":%s,"
            "\"has_voc_index\":%s,\"has_nox_index\":%s,"
            "\"pm_count\":%u,\"temperature_count\":%u,\"humidity_count\":%u,"
            "\"pressure_count\":%u,\"co2_count\":%u,"
            "\"voc_index_count\":%u,\"nox_index_count\":%u,"
            "\"timestamp_ms\":%lld}}",
            sample.pm1_0, sample.pm2_5, sample.pm4_0, sample.pm10_0,
            sample.nc0_5, sample.nc1_0, sample.nc2_5, sample.nc4_0,
            sample.nc10_0, sample.typical_particle_size,
            sample.temperature_c,
            sample.humidity_percent,
            sample.pressure_pa,
            sample.co2_ppm,
            sample.voc_index,
            sample.nox_index,
            sample.has_temperature ? "true" : "false",
            sample.has_humidity ? "true" : "false",
            sample.has_pressure ? "true" : "false",
            sample.has_co2 ? "true" : "false",
            sample.has_voc_index ? "true" : "false",
            sample.has_nox_index ? "true" : "false",
            sample.pm_count,
            sample.temperature_count,
            sample.humidity_count,
            sample.pressure_count,
            sample.co2_count,
            sample.voc_index_count,
            sample.nox_index_count,
            (long long)sample.timestamp_ms);
    } else {
        n = snprintf(buf, 4096, "null}");
    }

    if (n < 0 || n >= 4096) {
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
    httpd_resp_sendstr_chunk(req, "{\"interval_ms\":5000,\"samples\":[");

    uint16_t count = sensor_service_get_history_count();
    uint16_t emitted = 0;
    esp_err_t err = ESP_OK;
    for (uint16_t i = 0; i < count; i++) {
        sensor_history_point_t point;
        if (!sensor_service_get_history_point(i, &point)) {
            continue;
        }

        char item[512];
        int n = snprintf(item, sizeof(item),
                         "%s{\"t\":%lld,\"pm1\":%.2f,\"pm25\":%.2f,\"pm10\":%.2f,"
                         "\"temperature_c\":%.2f,\"humidity_percent\":%.2f,"
                         "\"pressure_pa\":%.2f,\"co2_ppm\":%u,"
                         "\"voc_index\":%.2f,\"nox_index\":%.2f,"
                         "\"has_temperature\":%s,\"has_humidity\":%s,"
                         "\"has_pressure\":%s,\"has_co2\":%s,"
                         "\"has_voc_index\":%s,\"has_nox_index\":%s}",
                         emitted ? "," : "",
                         (long long)point.timestamp_ms,
                         point.pm1_0,
                         point.pm2_5,
                         point.pm10_0,
                         point.temperature_c,
                         point.humidity_percent,
                         point.pressure_pa,
                         point.co2_ppm,
                         point.voc_index,
                         point.nox_index,
                         point.has_temperature ? "true" : "false",
                         point.has_humidity ? "true" : "false",
                         point.has_pressure ? "true" : "false",
                         point.has_co2 ? "true" : "false",
                         point.has_voc_index ? "true" : "false",
                         point.has_nox_index ? "true" : "false");
        if (n <= 0 || n >= (int)sizeof(item)) {
            err = ESP_FAIL;
            break;
        }
        err = httpd_resp_send_chunk(req, item, n);
        if (err != ESP_OK) {
            break;
        }
        emitted++;
    }

    if (err == ESP_OK) {
        err = httpd_resp_sendstr_chunk(req, "]}");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
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
    data_store_unlock();

    char head[1536];
    int n = snprintf(head, sizeof(head),
                     "{\"window_ms\":%lld,\"stored\":%lu,\"capacity\":%lu,"
                     "\"returned\":%lu,"
                     "\"active\":{\"end_epoch_ms\":%lld,\"end_uptime_ms\":%lld,"
                     "\"time_verified\":%s,\"time_reconciled\":%s,"
                     "\"time_quality\":\"%s\","
                     "\"frame_count\":%u,\"field_mask\":%u,"
                     "\"pm2_5_avg\":%.2f,\"pm2_5_min\":%.2f,\"pm2_5_max\":%.2f,\"pm2_5_count\":%u,"
                     "\"pm10_0_avg\":%.2f,\"pm10_0_min\":%.2f,\"pm10_0_max\":%.2f,\"pm10_0_count\":%u,"
                     "\"temperature_avg\":%.2f,\"temperature_min\":%.2f,\"temperature_max\":%.2f,\"temperature_count\":%u,"
                     "\"humidity_avg\":%.2f,\"humidity_min\":%.2f,\"humidity_max\":%.2f,\"humidity_count\":%u,"
                     "\"pressure_avg\":%.2f,\"pressure_min\":%.2f,\"pressure_max\":%.2f,\"pressure_count\":%u,"
                     "\"co2_avg\":%.2f,\"co2_min\":%.2f,\"co2_max\":%.2f,\"co2_count\":%u,"
                     "\"voc_index_avg\":%.2f,\"voc_index_min\":%.2f,\"voc_index_max\":%.2f,\"voc_index_count\":%u,"
                     "\"nox_index_avg\":%.2f,\"nox_index_min\":%.2f,\"nox_index_max\":%.2f,\"nox_index_count\":%u},"
                     "\"records\":[",
                     (long long)DATA_RECORD_WINDOW_MS,
                     (unsigned long)stored,
                     (unsigned long)capacity,
                     (unsigned long)returned,
                     (long long)active.end_epoch_ms,
                     (long long)active.end_uptime_ms,
                     active.time_verified ? "true" : "false",
                     active.time_reconciled ? "true" : "false",
                     time_quality_name(active.time_verified,
                                       active.time_reconciled),
                     active.frame_count,
                     active.field_mask,
                     active.pm2_5.avg, active.pm2_5.min, active.pm2_5.max, active.pm2_5.count,
                     active.pm10_0.avg, active.pm10_0.min, active.pm10_0.max, active.pm10_0.count,
                     active.temperature.avg, active.temperature.min, active.temperature.max, active.temperature.count,
                     active.humidity.avg, active.humidity.min, active.humidity.max, active.humidity.count,
                     active.pressure.avg, active.pressure.min, active.pressure.max, active.pressure.count,
                     active.co2.avg, active.co2.min, active.co2.max, active.co2.count,
                     active.voc_index.avg, active.voc_index.min, active.voc_index.max, active.voc_index.count,
                     active.nox_index.avg, active.nox_index.min, active.nox_index.max, active.nox_index.count);
    if (n <= 0 || n >= (int)sizeof(head)) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    esp_err_t err = httpd_resp_send_chunk(req, head, n);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t emitted = 0;
    for (uint32_t i = 0; err == ESP_OK && i < returned; i++) {
        data_record_t record;
        if (!data_store_lock(pdMS_TO_TICKS(500))) {
            return ESP_FAIL;
        }
        if (!data_store_get_record_locked(start + i, &record)) {
            data_store_unlock();
            continue;
        }
        data_store_unlock();

        char item[1536];
        n = format_record_json(item, sizeof(item), emitted ? "," : "",
                               &record);
        if (n <= 0 || n >= (int)sizeof(item)) {
            return ESP_FAIL;
        }

        err = httpd_resp_send_chunk(req, item, n);
        emitted++;
    }

    if (err != ESP_OK) {
        return err;
    }
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool export_query(httpd_req_t *req, bool *json_format, uint32_t *days) {
    *json_format = false;
    *days = 0;

    char query[96];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return true;
    }

    char format[12];
    if (httpd_query_key_value(query, "format", format, sizeof(format)) == ESP_OK) {
        if (strcmp(format, "json") == 0) {
            *json_format = true;
        } else if (strcmp(format, "csv") != 0) {
            return false;
        }
    }

    char days_str[16];
    if (httpd_query_key_value(query, "days", days_str, sizeof(days_str)) == ESP_OK) {
        char *end = NULL;
        unsigned long value = strtoul(days_str, &end, 10);
        if (end == days_str || *end != '\0' || value > 3650UL) {
            return false;
        }
        *days = (uint32_t)value;
    }

    return true;
}

static uint32_t export_start_index(uint32_t stored, uint32_t days) {
    if (days == 0) {
        return 0;
    }

    uint64_t range_ms = (uint64_t)days * (uint64_t)MS_PER_DAY;
    uint32_t max_records = (uint32_t)((range_ms + DATA_RECORD_WINDOW_MS - 1) /
                                      DATA_RECORD_WINDOW_MS);
    if (max_records == 0 || stored <= max_records) {
        return 0;
    }
    return stored - max_records;
}

static int format_record_json(char *buf, size_t size, const char *prefix,
                              const data_record_t *record) {
    return snprintf(buf, size,
                    "%s{\"seq\":%lu,\"end_epoch_ms\":%lld,\"end_uptime_ms\":%lld,"
                    "\"time_verified\":%s,\"time_reconciled\":%s,"
                    "\"time_quality\":\"%s\","
                    "\"frame_count\":%u,\"field_mask\":%u,"
                    "\"pm2_5_avg\":%.2f,\"pm2_5_min\":%.2f,\"pm2_5_max\":%.2f,\"pm2_5_count\":%u,"
                    "\"pm10_0_avg\":%.2f,\"pm10_0_min\":%.2f,\"pm10_0_max\":%.2f,\"pm10_0_count\":%u,"
                    "\"temperature_avg\":%.2f,\"temperature_min\":%.2f,\"temperature_max\":%.2f,\"temperature_count\":%u,"
                    "\"humidity_avg\":%.2f,\"humidity_min\":%.2f,\"humidity_max\":%.2f,\"humidity_count\":%u,"
                    "\"pressure_avg\":%.2f,\"pressure_min\":%.2f,\"pressure_max\":%.2f,\"pressure_count\":%u,"
                    "\"co2_avg\":%.2f,\"co2_min\":%.2f,\"co2_max\":%.2f,\"co2_count\":%u,"
                    "\"voc_index_avg\":%.2f,\"voc_index_min\":%.2f,\"voc_index_max\":%.2f,\"voc_index_count\":%u,"
                    "\"nox_index_avg\":%.2f,\"nox_index_min\":%.2f,\"nox_index_max\":%.2f,\"nox_index_count\":%u}",
                    prefix,
                    (unsigned long)record->seq,
                    (long long)record->end_epoch_ms,
                    (long long)record->end_uptime_ms,
                    record->time_verified ? "true" : "false",
                    record->time_reconciled ? "true" : "false",
                    time_quality_name(record->time_verified,
                                      record->time_reconciled),
                    record->frame_count,
                    record->field_mask,
                    record->pm2_5.avg, record->pm2_5.min, record->pm2_5.max, record->pm2_5.count,
                    record->pm10_0.avg, record->pm10_0.min, record->pm10_0.max, record->pm10_0.count,
                    record->temperature.avg, record->temperature.min, record->temperature.max, record->temperature.count,
                    record->humidity.avg, record->humidity.min, record->humidity.max, record->humidity.count,
                    record->pressure.avg, record->pressure.min, record->pressure.max, record->pressure.count,
                    record->co2.avg, record->co2.min, record->co2.max, record->co2.count,
                    record->voc_index.avg, record->voc_index.min, record->voc_index.max, record->voc_index.count,
                    record->nox_index.avg, record->nox_index.min, record->nox_index.max, record->nox_index.count);
}

static int format_record_csv(char *buf, size_t size, const data_record_t *record) {
    return snprintf(buf, size,
                    "%lu,%lld,%lld,%u,%u,%s,%u,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u,"
                    "%.2f,%.2f,%.2f,%u\n",
                    (unsigned long)record->seq,
                    (long long)record->end_epoch_ms,
                    (long long)record->end_uptime_ms,
                    record->time_verified ? 1u : 0u,
                    record->time_reconciled ? 1u : 0u,
                    time_quality_name(record->time_verified,
                                      record->time_reconciled),
                    record->frame_count,
                    record->field_mask,
                    record->pm2_5.avg, record->pm2_5.min, record->pm2_5.max, record->pm2_5.count,
                    record->pm10_0.avg, record->pm10_0.min, record->pm10_0.max, record->pm10_0.count,
                    record->temperature.avg, record->temperature.min, record->temperature.max, record->temperature.count,
                    record->humidity.avg, record->humidity.min, record->humidity.max, record->humidity.count,
                    record->pressure.avg, record->pressure.min, record->pressure.max, record->pressure.count,
                    record->co2.avg, record->co2.min, record->co2.max, record->co2.count,
                    record->voc_index.avg, record->voc_index.min, record->voc_index.max, record->voc_index.count,
                    record->nox_index.avg, record->nox_index.min, record->nox_index.max, record->nox_index.count);
}

static esp_err_t handler_export(httpd_req_t *req) {
    bool json_format = false;
    uint32_t days = 0;
    if (!export_query(req, &json_format, &days)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid export query");
        return ESP_FAIL;
    }

    if (!data_store_lock(pdMS_TO_TICKS(2000))) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "data store busy");
        return ESP_FAIL;
    }

    uint32_t stored = data_store_record_count_locked();
    uint32_t capacity = data_store_record_capacity_locked();
    uint32_t start = export_start_index(stored, days);
    uint32_t returned = stored - start;
    data_store_unlock();

    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition",
                       json_format ? "attachment; filename=\"air-history.json\""
                                   : "attachment; filename=\"air-history.csv\"");

    char *buf = malloc(2048);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    if (json_format) {
        httpd_resp_set_type(req, "application/json");
        int n = snprintf(buf, 2048,
                         "{\"window_ms\":%lld,\"stored\":%lu,\"capacity\":%lu,"
                         "\"returned\":%lu,\"days\":",
                         (long long)DATA_RECORD_WINDOW_MS,
                         (unsigned long)stored,
                         (unsigned long)capacity,
                         (unsigned long)returned);
        err = (n > 0 && n < 2048) ? httpd_resp_send_chunk(req, buf, n) : ESP_FAIL;
        if (err == ESP_OK) {
            if (days == 0) {
                err = httpd_resp_sendstr_chunk(req, "null,\"records\":[");
            } else {
                int n_days = snprintf(buf, 2048, "%lu,\"records\":[", (unsigned long)days);
                if (n_days > 0 && n_days < 2048) {
                    err = httpd_resp_send_chunk(req, buf, n_days);
                } else {
                    err = ESP_FAIL;
                }
            }
        }
    } else {
        httpd_resp_set_type(req, "text/csv; charset=utf-8");
        err = httpd_resp_sendstr_chunk(req,
            "seq,end_epoch_ms,end_uptime_ms,time_verified,time_reconciled,time_quality,frame_count,field_mask,"
            "pm2_5_avg,pm2_5_min,pm2_5_max,pm2_5_count,"
            "pm10_0_avg,pm10_0_min,pm10_0_max,pm10_0_count,"
            "temperature_avg,temperature_min,temperature_max,temperature_count,"
            "humidity_avg,humidity_min,humidity_max,humidity_count,"
            "pressure_avg,pressure_min,pressure_max,pressure_count,"
            "co2_avg,co2_min,co2_max,co2_count,"
            "voc_index_avg,voc_index_min,voc_index_max,voc_index_count,"
            "nox_index_avg,nox_index_min,nox_index_max,nox_index_count\n");
    }

    uint32_t emitted = 0;
    for (uint32_t i = start; err == ESP_OK && i < stored; i++) {
        data_record_t record;
        if (!data_store_lock(pdMS_TO_TICKS(500))) {
            err = ESP_FAIL;
            break;
        }
        if (!data_store_get_record_locked(i, &record)) {
            data_store_unlock();
            continue;
        }
        data_store_unlock();

        int n = json_format ? format_record_json(buf, 2048, emitted ? "," : "", &record)
                            : format_record_csv(buf, 2048, &record);
        if (n <= 0 || n >= 2048) {
            err = ESP_FAIL;
            break;
        }

        err = httpd_resp_send_chunk(req, buf, n);
        emitted++;
    }

    free(buf);

    if (err == ESP_OK && json_format) {
        err = httpd_resp_sendstr_chunk(req, "]}");
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t handler_ota(httpd_req_t *req) {
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty firmware image");
        return ESP_FAIL;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_FAIL;
    }

    if ((uint32_t)req->content_len > update_partition->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware image too large");
        return ESP_FAIL;
    }

    char *buf = malloc(OTA_RECV_BUF_SIZE);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(update_partition, req->content_len, &ota);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return err;
    }

    int remaining = req->content_len;
    int written = 0;
    while (remaining > 0) {
        int to_read = remaining > OTA_RECV_BUF_SIZE ? OTA_RECV_BUF_SIZE : remaining;
        int received = httpd_req_recv(req, buf, to_read);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "OTA receive failed: %d", received);
            break;
        }

        err = esp_ota_write(ota, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed after %d bytes: %s",
                     written, esp_err_to_name(err));
            break;
        }

        written += received;
        remaining -= received;
    }
    free(buf);

    if (err == ESP_OK && written == req->content_len) {
        err = esp_ota_end(ota);
        ota = 0;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
        }
    }

    if (ota != 0) {
        esp_ota_abort(ota);
    }

    if (err != ESP_OK || written != req->content_len) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "firmware validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA set boot partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA boot switch failed");
        return err;
    }

    ESP_LOGI(TAG, "OTA update accepted: partition=%s size=%d, rebooting",
             update_partition->label, written);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"firmware accepted, rebooting\"}");

    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
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
    httpd_config.max_uri_handlers = 8;
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
    const httpd_uri_t export = {
        .uri = "/api/export",
        .method = HTTP_GET,
        .handler = handler_export,
        .user_ctx = NULL,
    };
    const httpd_uri_t ota = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = handler_ota,
        .user_ctx = NULL,
    };
    const httpd_uri_t sensirion_logo = {
        .uri = "/assets/sensirion_logo.png",
        .method = HTTP_GET,
        .handler = handler_sensirion_logo,
        .user_ctx = NULL,
    };
    const httpd_uri_t bosch_logo = {
        .uri = "/assets/bosch_logo.svg",
        .method = HTTP_GET,
        .handler = handler_bosch_logo,
        .user_ctx = NULL,
    };

    const httpd_uri_t *handlers[] = {
        &root,
        &metrics,
        &hist,
        &data,
        &export,
        &ota,
        &sensirion_logo,
        &bosch_logo,
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        err = httpd_register_uri_handler(server, handlers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP handler registration failed for %s: %s",
                     handlers[i]->uri, esp_err_to_name(err));
            httpd_stop(server);
            server = NULL;
            return err;
        }
    }

    ESP_LOGI(TAG, "HTTP dashboard listening on port 80");
    return ESP_OK;
}
