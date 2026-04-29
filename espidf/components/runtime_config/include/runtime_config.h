#pragma once

#include "wifi_station.h"

#ifdef __cplusplus
extern "C" {
#endif

void runtime_config_load_wifi(wifi_station_config_t *config,
                              const char *fallback_ssid,
                              const char *fallback_password);

#ifdef __cplusplus
}
#endif
