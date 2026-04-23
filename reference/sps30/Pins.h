#pragma once

namespace HardwarePins {
constexpr int LCD_BL = 38;
constexpr int LCD_D0 = 39;
constexpr int LCD_D1 = 40;
constexpr int LCD_D2 = 41;
constexpr int LCD_D3 = 42;
constexpr int LCD_D4 = 45;
constexpr int LCD_D5 = 46;
constexpr int LCD_D6 = 47;
constexpr int LCD_D7 = 48;

constexpr int POWER_ON = 15;

constexpr int LCD_RES = 5;
constexpr int LCD_CS = 6;
constexpr int LCD_DC = 7;
constexpr int LCD_WR = 8;
constexpr int LCD_RD = 9;

constexpr int BUTTON_1 = 0;
constexpr int BUTTON_2 = 14;

constexpr int BAT_VOLT = 4;

constexpr int IIC_SCL = 17;
constexpr int IIC_SDA = 18;

constexpr int TOUCH_INT = 16;
constexpr int TOUCH_RES = 21;

constexpr int SD_CMD = 13;
constexpr int SD_CLK = 11;
constexpr int SD_D0 = 12;
}

namespace SensorPins {
constexpr int UART_RX = 16;
constexpr int UART_TX = 17;
constexpr int UART_BAUDRATE = 115200;
}
