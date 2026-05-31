# Firmware Architecture

The firmware is organized as small ESP-IDF components with explicit ownership.
New application components intentionally do not use a project-wide prefix.

## Runtime Flow

1. `main` initializes board power, display, NVS, storage, sensor sampling, Wi-Fi, HTTP, and tailnet.
2. `sensor_service` samples the active sensor drivers and keeps recent RAM history.
3. `data_store` receives samples through a callback and persists 10-minute aggregates to the `data` flash partition.
4. `runtime_config` loads runtime Wi-Fi configuration from the existing NVS schema
   and falls back to build-time credentials.
5. `wifi_station` owns station mode, reconnects, multi-network fallback, IP, and RSSI.
6. `tailnet_service` owns MicroLink startup, tailnet peer warm-up, and UDP diagnostics.
7. `web_server` serves the static dashboard and JSON APIs from service APIs.
8. `display_service` owns the local LilyGO T-Display-S3 LCD backend and small on-device status view.

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
  ESP-IDF rollback is enabled, so a newly booted OTA image must complete startup
  checks and mark itself valid or reboot within 3 minutes so the bootloader
  rolls back to the previous slot.
- `data_store` owns an append-only flash ring in the `data` partition. NVS is
  left for configuration, Wi-Fi, and MicroLink keys/peer cache.
- Flash-backed aggregate records store one aligned 10-minute bucket per record.
  Record version 5 covers the whole 256-byte flash record with CRC; version 4
  records remain readable for existing deployed data.
  After SNTP validates wall time, bucket boundaries are anchored to Unix epoch
  10-minute boundaries and the record's start can be derived from
  `end_epoch_ms - DATA_RECORD_WINDOW_MS`.
- If sampling starts before wall time is verified, `data_store` may keep an
  uptime-aligned active bucket for live summaries, but it does not persist that
  bucket to flash. Once SNTP verifies time, the unverified active bucket is
  dropped and new samples start in a fresh epoch-aligned bucket. This prefers
  small gaps over exporting records with a reordered or ambiguous timeline.
- On startup, `data_store` indexes only verified epoch records and ignores any
  legacy records that would make the exported sequence non-monotonic.
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
- `display_service` owns the LilyGO T-Display-S3 ST7789/I80 panel setup,
  backlight enablement, and a compact local status screen built from public
  service APIs. The GPIO14 side button cycles the display backlight through
  off, 30%, 70%, and 100%.

## Known Follow-Up Work

- Consider a formal sensor-driver registration API if more sensors are added so
  `sensor_service` does not keep accumulating direct driver knowledge.
- Continue refining local LCD layouts and interaction once the enclosure and
  normal viewing distance are known.
