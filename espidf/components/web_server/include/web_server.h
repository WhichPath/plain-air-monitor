#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;
} web_server_config_t;

esp_err_t web_server_start(const web_server_config_t *config);

#ifdef __cplusplus
}
#endif
