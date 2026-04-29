#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool synced;
    int64_t boot_epoch_ms;
    int64_t now_epoch_ms;
    uint32_t sync_count;
} time_service_snapshot_t;

void time_service_start(void);
bool time_service_is_synced(void);
bool time_service_get_snapshot(time_service_snapshot_t *out);
bool time_service_uptime_to_epoch_ms(int64_t uptime_ms, int64_t *out_epoch_ms);
int64_t time_service_now_epoch_ms(void);

#ifdef __cplusplus
}
#endif
