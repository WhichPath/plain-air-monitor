# Repository Guidance

This repository contains ESP-IDF firmware for a LilyGO T-Display-S3 air-quality sensor node.

## Build

- Use the workspace-local ESP-IDF environment when available:
  `source scripts/idf-env.sh`
- Build from the repository root:
  `idf.py -C espidf build`
- Real credentials stay in `espidf/sdkconfig.credentials`; do not commit that file.

## Version Control

- `initial-version` marks the original working firmware baseline.
- Do not commit `.tools/`, `espidf/build/`, generated `sdkconfig`, or real credentials.
- Keep refactors small enough that each commit can be reviewed against the previous behavior.

## Module Boundaries

- New application components should not use a project-wide prefix.
- `main` should only initialize services and wire them together.
- `sensor_service` owns sampling and recent in-memory history.
- `data_store` owns flash-backed aggregates and retention policy.
- `wifi_station` owns Wi-Fi station connection state.
- `tailnet_service` owns MicroLink/Tailscale and tailnet diagnostics.
- `web_server` owns HTTP routes and static dashboard assets.
- Low-level vendored code should remain isolated behind component APIs.

## Auditability

- Prefer explicit C structs and small public headers over hidden global cross-module access.
- Keep static web assets as normal files when possible, not escaped C strings.
- Avoid including internal component headers outside the adapter component that contains the dependency.
