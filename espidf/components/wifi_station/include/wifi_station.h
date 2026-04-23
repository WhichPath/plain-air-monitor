#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_STATION_MAX_NETWORKS 16

typedef struct {
    char ssid[33];
    char password[65];
} wifi_network_t;

typedef struct {
    wifi_network_t networks[WIFI_STATION_MAX_NETWORKS];
    size_t network_count;
} wifi_station_config_t;

esp_err_t station_start(const wifi_station_config_t *config);
esp_err_t station_wait_connected(TickType_t timeout_ticks);
void station_get_ip(char out[16]);
bool station_get_rssi(int *out_rssi);

#ifdef __cplusplus
}
#endif
