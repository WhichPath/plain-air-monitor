#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *auth_key;
    const char *device_name;
    const char *priority_peer_ip;
    const char *diagnostic_target_ip;
    uint8_t max_peers;
    int8_t wifi_tx_power_dbm;
} tailnet_config_t;

typedef struct {
    char vpn_ip[16];
    char state[20];
    int peer_count;
    bool connected;
} tailnet_status_t;

esp_err_t tailnet_service_start(const tailnet_config_t *config);
void tailnet_service_get_status(tailnet_status_t *out);

#ifdef __cplusplus
}
#endif
