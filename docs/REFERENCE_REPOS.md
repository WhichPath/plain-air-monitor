# Local Reference Repositories

Official upstream repositories are cloned under `reference/upstream/` for local
lookup while developing drivers and checking board details. The directory is
ignored by git because each entry is its own git checkout.

Refresh a checkout with:

```bash
git -C reference/upstream/<name> pull --ff-only
```

Current local checkouts:

| Local path | Upstream | Current HEAD | Use |
| --- | --- | --- | --- |
| `reference/upstream/lilygo-t-display-s3` | `https://github.com/Xinyuan-LilyGO/T-Display-S3.git` | `02f0e9d` | T-Display-S3 board, examples, and pin/reference material. |
| `reference/upstream/sensirion-embedded-uart-sps30` | `https://github.com/Sensirion/embedded-uart-sps30.git` | `09ca4ec` | SPS30 UART protocol and driver reference. |
| `reference/upstream/sensirion-embedded-i2c-sht4x` | `https://github.com/Sensirion/embedded-i2c-sht4x.git` | `1b6d714` | SHT4x/SHT45 I2C driver reference. |
| `reference/upstream/sensirion-embedded-i2c-scd4x` | `https://github.com/Sensirion/embedded-i2c-scd4x.git` | `743390b` | SCD4x/SCD41 CO2 I2C driver reference. |
| `reference/upstream/sensirion-embedded-i2c-sgp41` | `https://github.com/Sensirion/embedded-i2c-sgp41.git` | `c9a981e` | SGP41 raw VOC/NOx I2C driver reference. |
| `reference/upstream/sensirion-gas-index-algorithm` | `https://github.com/Sensirion/gas-index-algorithm.git` | `2ef9f13` | VOC Index and NOx Index algorithm reference for SGP41 output. |
| `reference/upstream/bosch-bmp5-sensor-api` | `https://github.com/boschsensortec/BMP5-Sensor-API.git` | `c779b44` | BMP581/BMP5 pressure sensor API reference. |

Keep model-specific repositories preferred over older family-level repositories
when porting code. For example, use `embedded-i2c-scd4x` for SCD41 rather than
the older `embedded-scd` repository, which is not the direct SCD41 reference.
