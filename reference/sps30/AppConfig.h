#pragma once

#include <array>
#include <cstdint>

namespace AppConfig {

namespace Timing {
constexpr unsigned long kReadIntervalMs = 1000;
constexpr uint8_t kSensorReadRetries = 3;
constexpr uint8_t kSensorRecoveryErrorThreshold = 3;
constexpr unsigned long kSensorRecoveryIntervalMs = 5000;
constexpr unsigned long kButtonDebounceMs = 50;
constexpr unsigned long kBrightnessIndicatorMs = 1500;
constexpr unsigned long kWiFiBlinkIntervalMs = 500;
constexpr unsigned long kLoopDelayMs = 10;
}

namespace Backlight {
constexpr uint8_t kDefaultLevel = 2;
constexpr uint8_t kPwmChannel = 0;
constexpr uint32_t kPwmFrequencyHz = 2000;
constexpr uint8_t kPwmResolutionBits = 8;
constexpr std::array<uint8_t, 4> kLevels = {0, 63, 127, 255};
constexpr std::array<uint8_t, 4> kPercents = {0, 25, 50, 100};
}

namespace Sensor {
constexpr uint8_t kValueCount = 4;
}

namespace WiFi {
constexpr char kSsid[] = "your-wifi-ssid";
constexpr char kPassword[] = "your-wifi-password";
constexpr bool kDisablePersistence = true;
constexpr unsigned long kConnectTimeoutMs = 10000;
constexpr unsigned long kReconnectRetryMs = 5000;
constexpr int32_t kStrongSignalThresholdDbm = -50;
}

namespace Mqtt {
constexpr bool kEnabled = false;
constexpr char kBroker[] = "192.168.1.100";
constexpr uint16_t kPort = 1883;
constexpr char kUsername[] = "";
constexpr char kPassword[] = "";
constexpr char kClientId[] = "sps30-pm-display";
constexpr char kTopicPrefix[] = "homeassistant/sensor/sps30";
constexpr uint16_t kSocketTimeoutMs = 1000;
constexpr uint16_t kSocketTimeoutSeconds = 1;
constexpr unsigned long kConnectRetryMs = 5000;
constexpr unsigned long kPublishIntervalMs = 30000;
}

struct PmThresholds {
    std::array<float, Sensor::kValueCount> level1;
    std::array<float, Sensor::kValueCount> level2;
    std::array<float, Sensor::kValueCount> level3;
    std::array<float, Sensor::kValueCount> level4;
};

constexpr PmThresholds kPmThresholds = {
    {5.0f, 8.0f, 15.0f, 25.0f},
    {10.0f, 15.0f, 30.0f, 50.0f},
    {20.0f, 25.0f, 60.0f, 100.0f},
    {35.0f, 40.0f, 100.0f, 150.0f},
};

namespace Colors {
constexpr uint16_t kGreen = 0x07E0;
constexpr uint16_t kYellow = 0xFFE0;
constexpr uint16_t kOrange = 0xFBE0;
constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kPurple = 0xF80F;
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kGray1 = 0xDEFB;
constexpr uint16_t kGray2 = 0xC618;
constexpr uint16_t kGray3 = 0xA514;
constexpr uint16_t kGray4 = 0x8410;
constexpr uint16_t kGray5 = 0x630C;
constexpr uint16_t kBlack = 0x0000;
constexpr uint16_t kLightPurple = 0xE39C;
constexpr uint16_t kCyan = 0x0555;
constexpr uint16_t kStatusGreen = 0x4C80;
constexpr uint16_t kStatusRed = 0xC008;
}

namespace Display {
constexpr int kScreenWidth = 320;
constexpr int kScreenHeight = 170;
constexpr int kWiFiIconX = 278;
constexpr int kWiFiIconY = 3;
constexpr int kWiFiIconWidth = 16;
constexpr int kWiFiIconHeight = 16;

namespace Layout {
constexpr int kOuterRectX = 21;
constexpr int kOuterRectY = 21;
constexpr int kOuterRectWidth = 278;
constexpr int kOuterRectHeight = 129;
constexpr int kOuterRectRadius = 7;

constexpr int kVerticalLine1X = 94;
constexpr int kVerticalLine2X = 222;
constexpr int kHorizontalLine1Y = 53;
constexpr int kHorizontalLine2Y = 85;
constexpr int kHorizontalLine3Y = 117;

constexpr int kValueAreaX = 98;
constexpr int kValueAreaY = 23;
constexpr int kValueAreaWidth = 125;
constexpr int kValueAreaHeight = 125;

constexpr std::array<int, Sensor::kValueCount> kCategoryLabelY = {31, 63, 95, 127};
constexpr int kCategoryLabelX = 26;
constexpr int kLevelLabelX = 176;

constexpr int kStackBarX = 235;
constexpr int kStackBarY = 32;
constexpr int kStackBarWidth = 38;
constexpr int kStackBarHeight = 107;
constexpr int kStackBarRadius = 7;

constexpr int kBrightnessIndicatorX = 26;
constexpr int kBrightnessIndicatorY = 7;
constexpr int kBrightnessIndicatorWidth = 28;
constexpr int kBrightnessIndicatorHeight = 11;

constexpr int kStatusAreaX = 0;
constexpr int kStatusAreaY = 154;
constexpr int kStatusAreaWidth = kScreenWidth;
constexpr int kStatusAreaHeight = 16;
constexpr int kStatusTextX = 16;
constexpr int kStatusTextY = 155;

constexpr int kErrorBoxX = 8;
constexpr int kErrorBoxY = 20;
constexpr int kErrorBoxWidth = kScreenWidth - 16;
constexpr int kErrorBoxHeight = 48;
}
}

}  // namespace AppConfig
