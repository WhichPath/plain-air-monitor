#include <Arduino.h>

#include "config/Pins.h"
#include "sensirion_common.h"
#include "sensirion_config.h"
#include "sensirion_uart_hal.h"

#ifndef SENSIRION_UART_BAUDRATE
#define SENSIRION_UART_BAUDRATE SensorPins::UART_BAUDRATE
#endif

#ifndef SENSIRION_UART_RX_TIMEOUT_MS
#define SENSIRION_UART_RX_TIMEOUT_MS 500
#endif

namespace {
HardwareSerial* uart = &Serial1;
bool uartInitialized = false;
}

extern "C" {

int16_t sensirion_uart_hal_init(UartDescr port) {
    (void)port;

    if (uartInitialized) {
        return 0;
    }

    uart->begin(SENSIRION_UART_BAUDRATE,
                SERIAL_8N1,
                SensorPins::UART_RX,
                SensorPins::UART_TX);
    uartInitialized = true;
    delay(10);
    return 0;
}

int16_t sensirion_uart_hal_free() {
    if (!uartInitialized) {
        return 0;
    }

    uart->end();
    uartInitialized = false;
    return 0;
}

int16_t sensirion_uart_hal_tx(uint16_t data_len, const uint8_t* data) {
    if (!uartInitialized) {
        return -1;
    }

    const size_t written = uart->write(data, data_len);
    uart->flush();
    return static_cast<int16_t>(written);
}

int16_t sensirion_uart_hal_rx(uint16_t max_data_len, uint8_t* data) {
    if (!uartInitialized) {
        return -1;
    }

    const uint32_t startMs = millis();
    while ((millis() - startMs) < SENSIRION_UART_RX_TIMEOUT_MS) {
        if (uart->available()) {
            break;
        }
        delay(1);
    }

    if (!uart->available()) {
        return 0;
    }

    uint16_t index = 0;
    constexpr uint32_t kInactivityTimeoutMs = 20;

    while (index < max_data_len) {
        if (!uart->available()) {
            break;
        }

        const int value = uart->read();
        if (value < 0) {
            break;
        }

        data[index++] = static_cast<uint8_t>(value);

        const uint32_t frameStartMs = millis();
        while ((millis() - frameStartMs) < kInactivityTimeoutMs) {
            if (uart->available()) {
                break;
            }
            delay(1);
        }

        if (!uart->available()) {
            break;
        }
    }

    return static_cast<int16_t>(index);
}

void sensirion_uart_hal_sleep_usec(uint32_t useconds) {
    if (useconds >= 1000) {
        delay(useconds / 1000);
    } else {
        delayMicroseconds(useconds);
    }
}

}  // extern "C"
