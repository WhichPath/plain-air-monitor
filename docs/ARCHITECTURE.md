# Firmware Architecture

The firmware is organized as small ESP-IDF components with explicit ownership.
New application components intentionally do not use a project-wide prefix.

## Runtime Flow

1. `main` initializes board power, NVS, storage, sensor sampling, Wi-Fi, HTTP, and tailnet.
2. `sensor_service` samples the active sensor drivers and keeps recent RAM history.
3. `data_store` receives samples through a callback and persists 10-minute aggregates to the `data` flash partition.
4. `runtime_config` loads runtime Wi-Fi configuration from the existing NVS schema
   and falls back to build-time credentials.
5. `wifi_station` owns station mode, reconnects, multi-network fallback, IP, and RSSI.
6. `tailnet_service` owns MicroLink startup, tailnet peer warm-up, and UDP diagnostics.
7. `web_server` serves the static dashboard and JSON APIs from service APIs.

## Current Component Boundaries

- `main` is orchestration only and does not read MicroLink runtime configuration
  internals directly.
- `runtime_config` is a thin compatibility boundary for runtime Wi-Fi settings.
  It preserves the current MicroLink NVS schema while keeping that detail out of
  the application entry point.
- `sensor_service` uses SPS30, SHT45, BMP581, SCD41, and SGP41 drivers
  internally. It samples SPS30/SHT45/SGP41 at 1 second, BMP581/SCD41 at 5
  seconds, and emits one 5-second unified frame for application consumers.
- The partition table uses two 4MB OTA app slots plus `otadata`; browser uploads
  write the inactive slot and switch boot only after ESP-IDF image validation.
- `data_store` owns an append-only flash ring in the `data` partition. NVS is
  left for configuration, Wi-Fi, and MicroLink keys/peer cache.
- Flash-backed aggregate records store one aligned 10-minute bucket per record.
  After SNTP validates wall time, bucket boundaries are anchored to Unix epoch
  10-minute boundaries and the record's start can be derived from
  `end_epoch_ms - DATA_RECORD_WINDOW_MS`.
- If sampling starts before wall time is verified, `data_store` still uses the
  ESP32 monotonic uptime clock to close approximate 10-minute buckets. These
  records carry `time_verified=false` and `end_uptime_ms`. Once SNTP later
  verifies time, `data_store` first closes any active uptime bucket, then starts
  a fresh epoch-aligned bucket for new samples. Pending unverified records are
  reconciled gradually by mapping their uptime bucket end back to epoch time,
  snapping to the real 10-minute bucket boundary, and marking the rewritten
  record with `time_verified=true` and `time_reconciled=true`.
- API/export records expose `time_quality`:
  `synced_epoch`, `reconciled_from_uptime`, or `unsynced_uptime`.
- Each field stores avg/min/max/count. Counts are the number of 5-second unified
  frames contributing to that field, so a fully covered 10-minute bucket is
  normally about 120 frames. Shorter counts indicate startup, reboot, sensor
  warm-up, missing readings, or a partial active bucket.
- Long-term fields are PM2.5, PM10, SHT45 temperature, SHT45 humidity, BMP581
  pressure, SCD41 CO2, SGP41 VOC Index, and SGP41 NOx Index.
- The dashboard's trend chart is based on flash-backed 10-minute aggregate records,
  not the live in-memory sample stream.
- `tailnet_service` uses the public MicroLink API for peer warm-up and transport
  health checks; application components should not include MicroLink internals.
- `web_server` serves the dashboard, JSON APIs, CSV/JSON data export, and the
  browser OTA upload endpoint. `web_server/dashboard.html` is embedded as a
  normal asset instead of escaped C text.

## Known Follow-Up Work

- Consider a formal sensor-driver registration API if more sensors are added so
  `sensor_service` does not keep accumulating direct driver knowledge.
- Add a display service stub before implementing the LilyGO T-Display-S3 UI.
