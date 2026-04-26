# Repository Guidance

This repository contains ESP-IDF firmware for a LilyGO T-Display-S3 air-quality sensor node.
The ESP-IDF project root is `espidf/`; repository-level docs live in `docs/`.

## Build

- Use the workspace-local ESP-IDF environment when available:
  `source scripts/idf-env.sh`
- Build from the repository root:
  `idf.py -C espidf build`
- If `espidf/sdkconfig.credentials` changes, force CMake to regenerate the private
  credentials header:
  `idf.py -C espidf reconfigure build`
- Real credentials stay in `espidf/sdkconfig.credentials`; do not commit that file.
- The target defaults are ESP32-S3, 16MB QIO flash, and OPI PSRAM, with a custom
  partition table in `espidf/partitions.csv`.

## Version Control

- `initial-version` marks the original working firmware baseline.
- Do not commit `.tools/`, `espidf/build/`, generated `sdkconfig`, package manager
  scratch files, or real credentials.
- Keep refactors small enough that each commit can be reviewed against the previous behavior.

## Module Boundaries

- New application components should not use a project-wide prefix.
- `main` should only initialize services and wire them together.
- `sensor_service` owns sampling and recent in-memory history.
- `data_store` owns flash-backed aggregates and retention policy.
- `wifi_station` owns Wi-Fi station connection state.
- `time_service` owns uptime-to-epoch conversion and time sync state.
- `tailnet_service` owns MicroLink/Tailscale and tailnet diagnostics.
- `web_server` owns HTTP routes and static dashboard assets.
- `sps30` is the current particulate sensor adapter and keeps Sensirion vendored
  code behind `pm_sps30.h`.
- `microlink` and `wireguard_lwip` are low-level network/tailnet components;
  application code should go through `tailnet_service` unless a boundary change is
  deliberate and documented.
- Low-level vendored code should remain isolated behind component APIs.

## Runtime Notes

- `web_server` serves `/`, `/api/metrics`, `/api/history`, and `/api/data`.
- `tailnet_service` exposes the diagnostic UDP echo on port `9000`.
- `data_store` persists completed 10-minute PM aggregates in the `data` partition;
  latest readings and recent fine-grained history remain RAM-only.
- NVS stores Wi-Fi, MicroLink/Tailscale keys, peer cache, and small configuration.
- SPS30 UART defaults are RX `GPIO16`, TX `GPIO17`, baud `115200`.

## Auditability

- Prefer explicit C structs and small public headers over hidden global cross-module access.
- Keep static web assets as normal files when possible, not escaped C strings.
- Avoid including internal component headers outside the adapter component that contains the dependency.
- Check `docs/ARCHITECTURE.md` and `docs/LOCAL_SETUP.md` when changing ownership,
  partitions, flashing behavior, or runtime endpoints.
