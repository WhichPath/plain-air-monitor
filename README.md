# PM MicroLink Linux Rebuild Seed

Goal: LilyGO T-Display-S3 joins Wi-Fi, connects to the tailnet with `microlink`, reads SPS30 particulate data, and serves a browser-rendered dashboard.

Included:
- `espidf/`: minimal ESP-IDF seed project
- `espidf/components/microlink/`: vendored `microlink` component
- `espidf/components/wireguard_lwip/`: dependency used by `microlink`
- `reference/sps30/`: SPS30 sensor integration reference
- `reference/board/`: board reference

Intentionally excluded:
- PlatformIO build output
- Windows-specific `ninja` and `pio` workarounds
- generated `.pio`, `.b`, or firmware artifacts

Linux bring-up:
1. Install ESP-IDF 5.x
2. Enter `espidf/`
3. Copy `sdkconfig.credentials.example` to `sdkconfig.credentials`
4. Fill real Wi-Fi credentials and Tailscale auth key
5. Run `idf.py set-target esp32s3 build`

This workspace also supports a fully local ESP-IDF setup under `.tools/`:

```bash
source scripts/idf-env.sh
idf.py -C espidf build
```

Runtime ports:
- Web dashboard: `80/tcp` at `http://<ESP32_TAILSCALE_IP>/`
- Metrics API: `80/tcp` at `/api/metrics`
- History API: `80/tcp` at `/api/history`
- Aggregate data API: `80/tcp` at `/api/data`
- UDP diagnostic echo: `9000/udp`

Recommended order:
1. Bring up `espidf/` first as a connectivity baseline
2. Port in `reference/sps30/`
3. Add your own web and history layer after tailnet connectivity is stable

Notes:
- `espidf/main/main.c` starts Wi-Fi, MicroLink, the SPS30 sampling task, a lightweight HTTP dashboard, and the UDP diagnostic echo.
- `sdkconfig.defaults` keeps the upstream config HTTP server disabled by default.
- `microlink` stores config and peer data in NVS. For a real deployment, plan around flash encryption.
- Recent raw samples are RAM/PSRAM-only. Completed 10-minute aggregates are stored in the dedicated `data` flash partition as an append-only ring.
