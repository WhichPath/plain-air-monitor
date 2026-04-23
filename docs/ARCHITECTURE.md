# Firmware Architecture

The firmware is organized as small ESP-IDF components with explicit ownership.
New application components intentionally do not use a project-wide prefix.

## Runtime Flow

1. `main` initializes board power, NVS, storage, sensor sampling, Wi-Fi, HTTP, and tailnet.
2. `sensor_service` samples the active sensor driver and keeps recent RAM history.
3. `data_store` receives samples through a callback and persists hourly aggregates to NVS.
4. `wifi_station` owns station mode, reconnects, multi-network fallback, IP, and RSSI.
5. `tailnet_service` owns MicroLink startup, tailnet peer warm-up, and UDP diagnostics.
6. `web_server` serves the static dashboard and JSON APIs from service APIs.

## Current Component Boundaries

- `main` is orchestration only.
- `sensor_service` still uses the SPS30 driver internally. The public API is generic enough
  to add a second driver or replace SPS30 without changing the web or storage layers.
- `data_store` keeps the existing NVS hourly ring behavior for compatibility.
  Flash-backed hourly records store PM1.0, PM2.5, PM4, and PM10 only.
- The dashboard's trend chart is based on flash-backed hourly PM records, not
  the live in-memory sample stream.
- `tailnet_service` is the only application component allowed to include MicroLink internals.
- `web_server/dashboard.html` is embedded as a normal asset instead of escaped C text.

## Known Follow-Up Work

- Replace NVS hourly-only storage with a capped flash data partition for detailed samples.
- Add a formal sensor-driver registration API before adding temperature/humidity sensors.
- Move MicroLink peer warm-up helpers into a public MicroLink API or remove the internal include.
- Add a display service stub before implementing the LilyGO T-Display-S3 UI.
