# Hardware BOM and Sensor Notes

This file records the current sensor hardware plan for the LilyGO T-Display-S3
air-quality node. It is a planning document, not a purchase lock.

## Confirmed Hardware

| Item | Status | Interface | Notes |
| --- | --- | --- | --- |
| LilyGO T-Display-S3 | Confirmed, headers soldered | USB-C power/data, UART, I2C/Qwiic | ESP32-S3 board. Current firmware enables board power on GPIO15. Board has one 4-pin Qwiic connector. |
| Sensirion SPS30 particulate sensor | Confirmed | UART, 5V power | Current firmware uses UART1 RX GPIO16, TX GPIO17, 115200 baud. Current cable is ZH 1.5mm 5-pin to Dupont, 20cm. |
| Sensirion SHT45 temperature/humidity sensor | Confirmed | I2C/Qwiic, 3.3V | Current firmware uses I2C0 SDA GPIO43, SCL GPIO44, address 0x44. Current cable is SH 1.0mm 4-pin to Dupont, 20cm; planned to move to Qwiic-to-Qwiic wiring. |

## Planned Sensors

| Item | Status | Interface | Primary Data | Notes |
| --- | --- | --- | --- | --- |
| Bosch BMP581 pressure sensor module | Confirmed, address verified | I2C/Qwiic, 3.3V | Barometric pressure, derived altitude | Verified I2C address 0x47. Preferred pressure choice for indoor/mobile use. It also reports on-chip temperature with about +/-0.5 K absolute accuracy, but this should not replace SHT45 ambient temperature. Use SHT45 as the single system temperature source. |
| Sensirion SCD41 CO2 sensor module | Confirmed, address verified | I2C/Qwiic, 3.3V | CO2 ppm | Verified I2C address 0x62. SCD41 also reports internal temperature/humidity for its own compensation. Do not expose those as primary temperature/humidity unless needed for diagnostics. If a Qwiic SCD43 module becomes readily available, SCD43 can be considered as a higher-accuracy CO2 alternative. |
| Sensirion SGP41 VOC/NOx sensor module | Confirmed, address verified | I2C/Qwiic/STEMMA QT, 3.3V | VOC Index, NOx Index | Verified I2C address 0x59. Output is index/trend data, not absolute ppm/ppb. Feed SHT45 temperature and humidity into the gas index compensation path. |

## Wiring Plan

- Power the whole node externally through the T-Display-S3 USB-C port.
- Keep SPS30 on its separate UART wiring and 5V supply path.
- Put SHT45, BMP581, SCD41, and SGP41 on the same I2C/Qwiic chain where possible.
- Qwiic chaining works by connecting board -> first sensor -> next sensor, when the
  breakout has two Qwiic connectors. The two connectors are normally the same I2C
  bus passed through, not separate ports.
- The board has one Qwiic port, so at least the first Qwiic cable starts from the
  board. Further modules can be daisy-chained from the second connector on each
  breakout if available.

Verified I2C addresses:

| Device | Address |
| --- | --- |
| SHT45 | 0x44 |
| BMP581 | 0x47 |
| SCD41 | 0x62 |
| SGP41 | 0x59 |

No address conflict is expected with this set.

## Cable and Mechanical Notes

- Planned Qwiic/STEMMA QT cable inventory:
  - 50mm Qwiic/QT to Qwiic/QT cable: 6 pieces.
  - 100mm Qwiic/QT to Qwiic/QT cable: 4 pieces.
- Current 20cm sensor cables are likely longer than needed for a mini enclosure.
- Shorter cables are usually better for both packaging and I2C signal margin.
- Final cable length should be determined after enclosure layout. Keep enough slack
  for assembly and service, but avoid coiling excess cable near sensor inlets.
- For Qwiic/I2C inside one small enclosure, target short cables first. 5-10cm is a
  reasonable starting range if the printed layout allows it.
- SPS30 needs airflow clearance and should not be pressed directly against walls or
  cable bundles. Leave a defined intake/exhaust path in the enclosure.
- SHT45 should be placed where it samples ambient air, away from ESP32, display,
  voltage regulators, and SPS30 motor heat.
- The pressure sensor needs air access but not direct water exposure. If the device
  may see condensation, rain mist, or outdoor dew, consider LPS28DFW instead of
  BMP581 for its water-resistant package.

## Data Ownership Rules

- Temperature and humidity: SHT45 is the primary source.
- BMP581 temperature: use only for BMP581 internal compensation/diagnostics, not as
  user-facing ambient temperature. Its datasheet temperature accuracy is good for
  a pressure sensor, but SHT45 is still the better ambient sensor and is less tied
  to BMP581 board placement.
- SCD41 temperature/humidity: use only for SCD41 diagnostics unless a specific
  calibration workflow needs them.
- SGP41 output: label as VOC Index and NOx Index. Do not label it as VOC ppm,
  TVOC ppb, or a regulatory AQI.
- CO2: SCD41 is the user-facing CO2 source.
- Pressure/altitude: BMP581 is the user-facing pressure source.

## Firmware Behavior

- A shared I2C bus component owns I2C0 on SDA GPIO43/SCL GPIO44.
- SPS30, SHT45, and SGP41 are sampled at 1 second. BMP581 and SCD41 are sampled
  at 5 seconds.
- `sensor_service` emits one unified 5-second frame. Faster samples are averaged
  into that frame.
- Flash stores aligned 10-minute aggregates. Long-term records include PM2.5,
  PM10, temperature, humidity, pressure, CO2, VOC Index, and NOx Index, with
  avg/min/max/count per field. Counts are 5-second unified frames; a full
  10-minute bucket is normally about 120 frames. PM1 and PM4 are not persisted
  long-term.
- If SNTP time is unavailable, aggregate buckets are temporarily aligned to
  ESP32 uptime and marked unverified. After time sync, pending unverified records
  are reconciled back to epoch-aligned 10-minute buckets.
- SHT45 temperature/humidity feeds SGP41 compensation. BMP581 pressure feeds SCD41
  ambient pressure compensation when available.

## Reference Links

Local official reference clones are indexed in `docs/REFERENCE_REPOS.md` and live
under `reference/upstream/`.

- LilyGO T-Display-S3: https://github.com/Xinyuan-LilyGO/T-Display-S3
- Sensirion SPS30: https://sensirion.com/products/catalog/SPS30
- Sensirion SHT45: https://sensirion.com/products/catalog/SHT45
- Bosch BMP581: https://www.bosch-sensortec.com/products/environmental-sensors/pressure-sensors/bmp581/
- Sensirion SCD41: https://sensirion.com/products/catalog/SCD41
- Sensirion SGP41: https://sensirion.com/products/catalog/SGP41
