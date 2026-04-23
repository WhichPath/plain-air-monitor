# SPS30 Porting Notes

- `SensorManager.*` is the existing read path reference.
- `sensirion_uart_hal_integration.cpp` is the UART HAL adapter.
- `Pins.h` is the T-Display-S3 pin reference.
- `lib_SPS30/` is the sensor protocol library.

This part is still Arduino/C++ oriented. For a clean ESP-IDF rebuild, keep the protocol and pin definitions and rewrite the state machine in your own style.
