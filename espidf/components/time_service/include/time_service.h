#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void time_service_start(void);
bool time_service_is_synced(void);
bool time_service_uptime_to_epoch_ms(int64_t uptime_ms, int64_t *out_epoch_ms);
int64_t time_service_now_epoch_ms(void);

#ifdef __cplusplus
}
#endif
