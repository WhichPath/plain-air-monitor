#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *device_name;
} display_service_config_t;

esp_err_t display_service_start(const display_service_config_t *config);

#ifdef __cplusplus
}
#endif
