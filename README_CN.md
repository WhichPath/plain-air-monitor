# 空气质量监测器 (Plain Air Monitor)

基于LilyGO T-Display-S3的微气候监测系统，使用ESP-IDF框架通过Tailscale网络进行空气质量监测。

## 当前状态

**当前支持的传感器：**
- **Sensirion SPS30** - 颗粒物传感器 (PM1.0, PM2.5, PM4.0, PM10)
- **Sensirion SHT45** - 温湿度传感器
- **Sensirion SCD41** - CO2浓度传感器
- **Sensirion SGP41** - VOC和NOx传感器
- **Bosch BMP581** - 气压传感器

**硬件平台：** LilyGO T-Display-S3 (ESP32-S3, 16MB Flash, 8MB PSRAM)

**内置显示屏：** 紧凑的本机状态界面，用于显示实时读数、网络状态、时间同步和亮度状态。

## 显示预览

![T-Display-S3 本机显示预览](docs/images/display-preview.png)

## 项目目标

### 近期目标
- 继续优化本机 LCD 布局和备用显示模式
- 在外壳和安装方式确定后调整板载交互

### 远期目标
- 设计并3D打印定制外壳以容纳所有组件
- 创建完整的、可部署的空气质量监测站
- 添加对更多环境传感器的支持
- 扩展历史数据分析和运行诊断能力

## 功能特性

- **Tailscale集成**：通过Tailscale网络实现安全的远程访问
- **Web仪表板**：基于浏览器的实时数据可视化
- **本机 LCD 显示**：在设备上显示 PM2.5、PM10、温湿度、CO2、VOC、NOx 和连接状态
- **GPIO14 亮度按钮**：轮换内置显示屏的关闭、30%、70% 和 100% 亮度
- **OTA更新**：通过Web界面进行固件空中升级
- **数据存储**：10分钟聚合数据存储在闪存中
- **双OTA槽**：固件更新使用 ESP-IDF rollback，并在启动检查通过后确认新固件

## 硬件要求

- LilyGO T-Display-S3开发板
- Sensirion SPS30颗粒物传感器
- Sensirion SHT45温湿度传感器
- Sensirion SCD41 CO2传感器（可选）
- Sensirion SGP41 VOC/NOx传感器（可选）
- Bosch BMP581气压传感器（可选）
- Qwiic/Stemma QT连接线用于I2C连接

## 快速开始

### 前置条件

1. 安装ESP-IDF 5.x
2. 克隆此仓库
3. 设置凭证（见下文）

### 配置

1. 复制凭证示例文件：
   ```bash
   cp espidf/sdkconfig.credentials.example espidf/sdkconfig.credentials
   ```

2. 编辑 `espidf/sdkconfig.credentials` 文件，填入您的实际值：
   ```
   CONFIG_ML_WIFI_SSID="your-wifi-ssid"
   CONFIG_ML_WIFI_PASSWORD="your-wifi-password"
   CONFIG_ML_TAILSCALE_AUTH_KEY="tskey-auth-your-actual-key"
   ```

3. 编译并烧录：
   ```bash
   idf.py -C espidf build
   idf.py -C espidf flash monitor
   ```

### 本地开发设置

此项目支持完全本地的ESP-IDF设置：

```bash
source scripts/idf-env.sh
idf.py -C espidf build
```

## API端点

- **Web仪表板**: `http://<ESP32_LAN_OR_TAILSCALE_IP>/` (端口80)
- **指标API**: `/api/metrics`
- **历史API**: `/api/history`
- **聚合数据**: `/api/data`
- **UDP诊断**: 端口9000

## 项目结构

```
plain-air-monitor/
├── espidf/                    # 主固件项目
│   ├── components/            # ESP-IDF组件
│   │   ├── bmp581/           # BMP581气压传感器驱动
│   │   ├── data_store/       # 数据存储管理
│   │   ├── display_service/  # 内置 LCD 状态显示
│   │   ├── i2c_bus/          # I2C总线管理
│   │   ├── microlink/        # Tailscale集成
│   │   ├── runtime_config/   # 运行时配置
│   │   ├── scd41/            # SCD41 CO2传感器驱动
│   │   ├── sensor_service/   # 传感器管理服务
│   │   ├── sgp41/            # SGP41 VOC/NOx传感器驱动
│   │   ├── sht45/            # SHT45温湿度驱动
│   │   ├── sps30/            # SPS30颗粒物驱动
│   │   ├── tailnet_service/  # Tailscale网络服务
│   │   ├── time_service/     # 时间同步
│   │   ├── web_server/       # HTTP服务器
│   │   ├── wifi_station/     # WiFi连接管理
│   │   └── wireguard_lwip/   # WireGuard for Tailscale
│   └── main/                 # 应用程序入口点
├── reference/                 # 参考资料
│   ├── board/                # 板子配置文件
│   ├── box-design/           # 外壳设计图片
│   ├── sps30/                # SPS30遗留参考
│   └── upstream/             # 上游仓库引用（已忽略）
├── docs/                     # 文档
└── scripts/                  # 开发脚本
```

## 许可证

此项目使用BSD 3-Clause许可证 - 详见 [LICENSE](LICENSE) 文件。

### 第三方组件

- **microlink**: BSD 3-Clause许可证 (Tailscale集成)
- **Sensirion驱动**: BSD 3-Clause许可证
- **Bosch BMP5-Sensor-API**: BSD 3-Clause许可证

## 贡献

1. Fork此仓库
2. 创建功能分支
3. 提交您的更改
4. 推送到分支
5. 创建Pull Request

## 致谢

- [LilyGO](https://www.lilygo.cc/) 提供T-Display-S3硬件
- [Sensirion](https://sensirion.com/) 提供传感器库和参考实现
- [Bosch Sensortec](https://www.bosch-sensortec.com/) 提供BMP581传感器API
- [Tailscale](https://tailscale.com/) 提供安全网络解决方案

## 联系方式

如有问题和支持需求，请在GitHub上创建issue。
