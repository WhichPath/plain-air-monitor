#include "sensirion_uart_hal.h"
#include "pm_sps30.h"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define RX_BUF_SIZE 256
#define TX_BUF_SIZE 0
#define RX_TIMEOUT_MS 500

static bool s_uart_initialized;

int16_t sensirion_uart_hal_init(UartDescr port) {
    (void)port;

    if (s_uart_initialized) {
        return 0;
    }

    uart_config_t cfg = {
        .baud_rate = PM_SPS30_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(PM_SPS30_UART_NUM, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return (int16_t)err;
    }

    err = uart_param_config(PM_SPS30_UART_NUM, &cfg);
    if (err != ESP_OK) {
        return (int16_t)err;
    }

    err = uart_set_pin(PM_SPS30_UART_NUM,
                       PM_SPS30_UART_TX_GPIO,
                       PM_SPS30_UART_RX_GPIO,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return (int16_t)err;
    }

    uart_flush_input(PM_SPS30_UART_NUM);
    s_uart_initialized = true;
    vTaskDelay(pdMS_TO_TICKS(10));
    return 0;
}

int16_t sensirion_uart_hal_free(void) {
    if (!s_uart_initialized) {
        return 0;
    }

    uart_driver_delete(PM_SPS30_UART_NUM);
    s_uart_initialized = false;
    return 0;
}

int16_t sensirion_uart_hal_tx(uint16_t data_len, const uint8_t *data) {
    if (!s_uart_initialized || !data) {
        return -1;
    }

    int written = uart_write_bytes(PM_SPS30_UART_NUM, data, data_len);
    if (written < 0) {
        return -1;
    }
    uart_wait_tx_done(PM_SPS30_UART_NUM, pdMS_TO_TICKS(100));
    return (int16_t)written;
}

int16_t sensirion_uart_hal_rx(uint16_t max_data_len, uint8_t *data) {
    if (!s_uart_initialized || !data || max_data_len == 0) {
        return -1;
    }

    int total = 0;
    int first = uart_read_bytes(PM_SPS30_UART_NUM, data, 1, pdMS_TO_TICKS(RX_TIMEOUT_MS));
    if (first <= 0) {
        return 0;
    }
    total = first;

    while (total < max_data_len) {
        int n = uart_read_bytes(PM_SPS30_UART_NUM,
                                data + total,
                                max_data_len - total,
                                pdMS_TO_TICKS(20));
        if (n <= 0) {
            break;
        }
        total += n;
    }

    return (int16_t)total;
}

void sensirion_uart_hal_sleep_usec(uint32_t useconds) {
    if (useconds >= 1000) {
        vTaskDelay(pdMS_TO_TICKS((useconds + 999) / 1000));
    } else {
        esp_rom_delay_us(useconds);
    }
}
