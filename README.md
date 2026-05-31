# Plain Air Monitor

A microclimate monitoring system based on LilyGO T-Display-S3, using ESP-IDF framework for air quality monitoring through Tailscale network.

## Current Status

**Currently supported sensors:**
- **Sensirion SPS30** - Particulate matter sensor (PM1.0, PM2.5, PM4.0, PM10)
- **Sensirion SHT45** - Temperature and humidity sensor
- **Sensirion SCD41** - CO2 concentration sensor
- **Sensirion SGP41** - VOC and NOx sensor
- **Bosch BMP581** - Barometric pressure sensor

**Hardware platform:** LilyGO T-Display-S3 (ESP32-S3 with 16MB Flash, 8MB PSRAM)

## Project Goals

### Short-term Goals
- Utilize the built-in TFT display on T-Display-S3 to create an information display interface
- Show real-time sensor data, network status, and system information on the screen
- Implement touch interaction for switching display modes

### Long-term Goals
- Design and 3D print a custom enclosure to house all components
- Create a complete, deployable air quality monitoring station
- Add support for additional environmental sensors
- Implement data logging and historical analysis features

## Features

- **Tailscale Integration**: Secure remote access via Tailscale network
- **Web Dashboard**: Browser-based real-time data visualization
- **OTA Updates**: Over-the-air firmware updates via web interface
- **Data Storage**: 10-minute aggregated data stored in flash memory
- **Dual OTA**: Safe firmware updates with ESP-IDF rollback after startup validation

## Hardware Requirements

- LilyGO T-Display-S3 development board
- Sensirion SPS30 particulate matter sensor
- Sensirion SHT45 temperature/humidity sensor
- Sensirion SCD41 CO2 sensor (optional)
- Sensirion SGP41 VOC/NOx sensor (optional)
- Bosch BMP581 barometric pressure sensor (optional)
- Qwiic/Stemma QT cables for I2C connections

## Getting Started

### Prerequisites

1. Install ESP-IDF 5.x
2. Clone this repository
3. Set up credentials (see below)

### Configuration

1. Copy the credentials example file:
   ```bash
   cp espidf/sdkconfig.credentials.example espidf/sdkconfig.credentials
   ```

2. Edit `espidf/sdkconfig.credentials` with your actual values:
   ```
   CONFIG_ML_WIFI_SSID="your-wifi-ssid"
   CONFIG_ML_WIFI_PASSWORD="your-wifi-password"
   CONFIG_ML_TAILSCALE_AUTH_KEY="tskey-auth-your-actual-key"
   ```

3. Build and flash:
   ```bash
   idf.py -C espidf build
   idf.py -C espidf flash monitor
   ```

### Local Development Setup

This project supports a fully local ESP-IDF setup:

```bash
source scripts/idf-env.sh
idf.py -C espidf build
```

## API Endpoints

- **Web Dashboard**: `http://<ESP32_TAILSCALE_IP>/` (Port 80)
- **Metrics API**: `/api/metrics`
- **History API**: `/api/history`
- **Aggregate Data**: `/api/data`
- **UDP Diagnostic**: Port 9000

## Project Structure

```
plain-air-monitor/
├── espidf/                    # Main firmware project
│   ├── components/            # ESP-IDF components
│   │   ├── bmp581/           # BMP581 pressure sensor driver
│   │   ├── data_store/       # Data storage management
│   │   ├── i2c_bus/          # I2C bus management
│   │   ├── microlink/        # Tailscale integration
│   │   ├── runtime_config/   # Runtime configuration
│   │   ├── scd41/            # SCD41 CO2 sensor driver
│   │   ├── sensor_service/   # Sensor management service
│   │   ├── sgp41/            # SGP41 VOC/NOx sensor driver
│   │   ├── sht45/            # SHT45 temp/humidity driver
│   │   ├── sps30/            # SPS30 particulate matter driver
│   │   ├── tailnet_service/  # Tailscale network service
│   │   ├── time_service/     # Time synchronization
│   │   ├── web_server/       # HTTP server
│   │   ├── wifi_station/     # WiFi connection management
│   │   └── wireguard_lwip/   # WireGuard for Tailscale
│   └── main/                 # Application entry point
├── reference/                 # Reference materials
│   ├── board/                # Board configuration files
│   ├── box-design/           # Enclosure design images
│   ├── sps30/                # SPS30 legacy reference
│   └── upstream/             # Upstream repository references (gitignored)
├── docs/                     # Documentation
└── scripts/                  # Development scripts
```

## License

This project is licensed under the BSD 3-Clause License - see the [LICENSE](LICENSE) file for details.

### Third-party Components

- **microlink**: BSD 3-Clause License (Tailscale integration)
- **Sensirion drivers**: BSD 3-Clause License
- **Bosch BMP5-Sensor-API**: BSD 3-Clause License

## Contributing

1. Fork the repository
2. Create a feature branch
3. Commit your changes
4. Push to the branch
5. Create a Pull Request

## Acknowledgments

- [LilyGO](https://www.lilygo.cc/) for the T-Display-S3 hardware
- [Sensirion](https://sensirion.com/) for sensor libraries and reference implementations
- [Bosch Sensortec](https://www.bosch-sensortec.com/) for BMP581 sensor API
- [Tailscale](https://tailscale.com/) for secure networking solution

## Contact

For questions and support, please open an issue on GitHub.
