#ifndef SENSIRION_UART_HAL_H
#define SENSIRION_UART_HAL_H

#include "sensirion_config.h"
#include "sensirion_uart_portdescriptor.h"

#ifdef __cplusplus
extern "C" {
#endif

int16_t sensirion_uart_hal_init(UartDescr port);
int16_t sensirion_uart_hal_free();
int16_t sensirion_uart_hal_tx(uint16_t data_len, const uint8_t* data);
int16_t sensirion_uart_hal_rx(uint16_t max_data_len, uint8_t* data);
void sensirion_uart_hal_sleep_usec(uint32_t useconds);

#ifdef __cplusplus
}
#endif

#endif
