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

If `scripts/idf-env.sh` reports a missing ESP-IDF tool, refresh the local tool
install and then source the environment again:

```bash
IDF_TOOLS_PATH="$PWD/.tools/espressif" .tools/esp-idf/install.sh
source scripts/idf-env.sh
```

If a new `sdkconfig.defaults` option does not appear in an older local
`espidf/sdkconfig`, delete the ignored generated `espidf/sdkconfig` and rebuild
so ESP-IDF regenerates it from defaults and credentials.

Set credentials for real device bring-up:

```bash
cp espidf/sdkconfig.credentials.example espidf/sdkconfig.credentials
$EDITOR espidf/sdkconfig.credentials
```

The firmware reads `sdkconfig.credentials` at compile time through a generated private header. After editing that file, rebuild with reconfigure so CMake regenerates the private header:

```bash
idf.py -C espidf reconfigure build
```

At runtime, Wi-Fi settings are loaded through the `runtime_config` component. It
keeps compatibility with the existing MicroLink NVS Wi-Fi keys and uses
`sdkconfig.credentials` only as the build-time fallback.

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
- Aggregate data API: `/api/data`
- Data export API: `/api/export?format=csv|json[&days=N]`
- Browser OTA upload API: `/api/ota`
- Diagnostic UDP echo: `<ESP32_TAILSCALE_IP>:9000/udp`

Storage behavior:
- Latest sample and fine-grained recent history stay in RAM/PSRAM only.
- Browser-side charts also keep session-only data in the requesting device's memory.
- Completed 10-minute sensor aggregates are persisted to the ESP32 `data` flash partition.
- The `data` partition is an append-only ring. NVS remains for Wi-Fi, MicroLink keys, peer cache, and small configuration.
- Aggregate records store PM2.5, PM10, SHT45 temperature/humidity, BMP581
  pressure, SCD41 CO2, and SGP41 VOC/NOx avg/min/max/count plus field flags.
- After wall time is verified by SNTP, aggregate buckets use Unix epoch-aligned
  10-minute boundaries. Before time is verified, the firmware still closes
  approximate 10-minute buckets from ESP32 uptime and marks them
  `time_verified=false`; after time sync, the active uptime bucket is closed
  before new synced samples enter an epoch bucket. Pending unverified records
  are reconciled back to epoch buckets gradually and marked
  `time_reconciled=true`.
- API/export records include `time_quality`: `synced_epoch`,
  `reconciled_from_uptime`, or `unsynced_uptime`.
- Aggregate `count` values are 5-second unified frames, not raw 1-second sensor
  reads. A full 10-minute bucket is normally about 120 frames; smaller counts
  mean startup/reboot, a partial active bucket, sensor warm-up, or missed fields.

The firmware uses a custom 16MB OTA partition table: 256KB NVS, 8KB `otadata`,
two 4MB OTA app slots, and a 0x7B0000-byte `data` partition. ESP-IDF rollback is
enabled; a newly uploaded app is marked valid only after NVS, storage, Wi-Fi,
HTTP, and tailnet service startup checks pass. If those checks do not complete
within 3 minutes, the app reboots so the bootloader can roll back. When moving from
the earlier single-app layout to this layout, erase flash once:

```bash
idf.py -C espidf -p /dev/ttyACM0 erase-flash flash monitor
```

`erase-flash` clears NVS and the `data` partition, including the MicroLink/Tailscale machine identity and stored aggregate records. If the Tailscale auth key is one-time use, generate a fresh reusable or ephemeral auth key before rebuilding after an erase.

To reset only the stored aggregate records while preserving NVS credentials and
MicroLink identity, erase the `data` partition region from `espidf/partitions.csv`:

```bash
python -m esptool --chip esp32s3 -p /dev/ttyACM0 erase_region 0x850000 0x7B0000
```

SPS30 wiring defaults:
- Sensor UART RX on ESP32-S3: `GPIO16`
- Sensor UART TX on ESP32-S3: `GPIO17`
- Baud rate: `115200`

SHT45 wiring defaults:
- I2C bus: `I2C0`
- SDA: `GPIO43`
- SCL: `GPIO44`
- Address: `0x44`
- Bus speed: `100000`

Shared I2C sensor addresses:
- SHT45: `0x44`
- BMP581: `0x47`
- SCD41: `0x62`
- SGP41: `0x59`

The web page is static HTML/CSS/JS served by the ESP32. Chart rendering and UI
updates run in the requesting browser. The ESP32 samples SPS30, SHT45, BMP581,
SCD41, and SGP41 data, keeps a small in-memory history ring, and returns JSON.

Current verification:
- `idf.py -C espidf build` completed successfully.
- Output binary: `espidf/build/ml_seed.bin`
- Build warnings are currently limited to unused variables/functions in vendored MicroLink/WireGuard sources.
- T-Display-S3 flash defaults are set in `espidf/sdkconfig.defaults` as 16MB, QIO, 80MHz, with OPI PSRAM enabled.
- On the attached ESP32-S3, local Wi-Fi dashboard/API responded on port 80 at `192.168.1.200`.
- SPS30, SHT45, BMP581, SCD41, and SGP41 were verified through `/api/metrics`;
  all five report `measuring` after startup and sensor conditioning completes.
- `/api/data` exposes flash-backed 10-minute aggregates plus the current active
  bucket, including `time_verified`, `time_reconciled`, `time_quality`, and
  `end_uptime_ms` for bucket provenance.
- The mobile dashboard was checked with an iPhone 15 browser viewport after
  flashing. The pressure card fits `1016.x hPa`, pressure chart ticks render
  fully, and pressure chart values are displayed in hPa.
