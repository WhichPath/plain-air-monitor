# Local Workspace Setup

This workspace is prepared for the LilyGO T-Display-S3 target board.

Board facts checked against the official LilyGO repository:
- Product: T-Display-S3
- SoC: ESP32-S3R8
- Flash: 16MB
- PSRAM: 8MB OPI
- Display: 170x320, 1.9 inch
- Reference: https://github.com/Xinyuan-LilyGO/T-Display-S3

Local components prepared:
- ESP-IDF: `.tools/esp-idf`, tag `v5.3.5`
- ESP-IDF tools: `.tools/espressif`
- Project: `espidf/`
- Vendored project components:
  - `espidf/components/microlink`
  - `espidf/components/wireguard_lwip`

Use the local ESP-IDF environment:

```bash
source scripts/idf-env.sh
idf.py -C espidf build
```

Set credentials for real device bring-up:

```bash
cp espidf/sdkconfig.credentials.example espidf/sdkconfig.credentials
$EDITOR espidf/sdkconfig.credentials
```

The firmware reads `sdkconfig.credentials` at compile time through a generated private header. After editing that file, rebuild with reconfigure so CMake regenerates the private header:

```bash
idf.py -C espidf reconfigure build
```

Flash and monitor, replacing the serial port as needed:

```bash
idf.py -C espidf -p /dev/ttyACM0 flash monitor
```

Exit `idf.py monitor` with `Ctrl+]`. If your terminal intercepts that, use `Ctrl+T` then `Ctrl+X`.

Runtime services:
- Dashboard over Tailscale: `http://<ESP32_TAILSCALE_IP>/`
- Dashboard over local Wi-Fi: `http://<ESP32_WIFI_IP>/`
- Dashboard port: `80/tcp`
- Metrics API: `/api/metrics`
- Recent in-memory history API: `/api/history`
- Hourly average API: `/api/hourly`
- Diagnostic UDP echo: `<ESP32_TAILSCALE_IP>:9000/udp`

Storage behavior:
- Latest sample and fine-grained recent history stay in RAM/PSRAM only.
- Browser-side charts also keep session-only data in the requesting device's memory.
- Completed hourly PM averages are persisted to ESP32 NVS flash.
- NVS uses ESP-IDF's wear-levelled flash storage; firmware writes hourly records only when an hour closes, not every sample.
- Current hourly capacity is 168 records.

The firmware uses a custom partition table with a larger NVS partition. When moving from the earlier build to this layout, erase flash once:

```bash
idf.py -C espidf -p /dev/ttyACM0 erase-flash flash monitor
```

`erase-flash` clears NVS, including the MicroLink/Tailscale machine identity and stored hourly records. If the Tailscale auth key is one-time use, generate a fresh reusable or ephemeral auth key before rebuilding after an erase.

SPS30 wiring defaults:
- Sensor UART RX on ESP32-S3: `GPIO16`
- Sensor UART TX on ESP32-S3: `GPIO17`
- Baud rate: `115200`

The web page is static HTML/CSS/JS served by the ESP32. Chart rendering and UI updates run in the requesting browser. The ESP32 only samples SPS30 data, keeps a small in-memory history ring, and returns JSON.

Current verification:
- `idf.py -C espidf build` completed successfully.
- Output binary: `espidf/build/ml_seed.bin`
- Build warnings are currently limited to unused variables/functions in vendored MicroLink/WireGuard sources.
- T-Display-S3 flash defaults are set in `espidf/sdkconfig.defaults` as 16MB, QIO, 80MHz, with OPI PSRAM enabled.
- On the attached ESP32-S3, local Wi-Fi dashboard/API responded on port 80 at `192.168.1.196`.
- SPS30 readout was verified through `/api/metrics` and `/api/history` with `sensor.state=measuring`, increasing `read_count`, and `error_count=0`.
