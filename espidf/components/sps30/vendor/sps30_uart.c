/*
 * SPS30 driver (copied from Sensirion embedded-uart-sps30)
 */

#include "sps30_uart.h"
#include "sensirion_common.h"
#include "sensirion_streaming_shdlc.h"
#include "sensirion_uart_hal.h"

/* Extern: last header populated by SHDLC layer on error paths. */
extern struct sensirion_shdlc_rx_header sensirion_shdlc_last_header;

#define sensirion_hal_sleep_us sensirion_uart_hal_sleep_usec

static uint8_t communication_buffer[44] = {0};

int16_t sps30_wake_up_sequence() {
    int16_t local_error = 0;
    /* Robust wake up sequence per datasheet:
       Try wake_up_communication() (single 0xFF) first; if that fails,
       fall back to sending wake_up() commands (0x11) twice as the
       datasheet suggests as an alternative. */
    int16_t comm_err = sps30_wake_up_communication();
    (void)comm_err; /* we continue regardless, try wake commands */
    local_error = sps30_wake_up();
    if (local_error != NO_ERROR) {
        /* try a second wake command as fallback */
        local_error = sps30_wake_up();
    }
    return local_error;
}

int16_t sps30_start_measurement(sps30_output_format measurement_output_format) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x0, SPS30_SHDLC_ADDR, 2);
    sensirion_add_uint16_t_argument(&stream, measurement_output_format);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    /* If start_measurement returns execution error 0x43 (command not
       allowed in current state), the device may already be measuring. Treat
       this as success so higher-level code can continue. */
    if (local_error == SENSIRION_SHDLC_ERR_EXECUTION_FAILURE) {
        if (sensirion_shdlc_last_header.cmd == 0x00 &&
            (sensirion_shdlc_last_header.state & 0x7F) == 0x43) {
            local_error = NO_ERROR;
        }
    }
    return local_error;
}

int16_t sps30_stop_measurement() {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x1, SPS30_SHDLC_ADDR, 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    return local_error;
}

int16_t sps30_read_measurement_values_float(float* mc_1p0, float* mc_2p5,
                                            float* mc_4p0, float* mc_10p0,
                                            float* nc_0p5, float* nc_1p0,
                                            float* nc_2p5, float* nc_4p0,
                                            float* nc_10p0,
                                            float* typical_particle_size) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x3, SPS30_SHDLC_ADDR, 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 40, &header, 50);
    *mc_1p0 = sensirion_common_bytes_to_float(&buffer_ptr[0]);
    *mc_2p5 = sensirion_common_bytes_to_float(&buffer_ptr[4]);
    *mc_4p0 = sensirion_common_bytes_to_float(&buffer_ptr[8]);
    *mc_10p0 = sensirion_common_bytes_to_float(&buffer_ptr[12]);
    *nc_0p5 = sensirion_common_bytes_to_float(&buffer_ptr[16]);
    *nc_1p0 = sensirion_common_bytes_to_float(&buffer_ptr[20]);
    *nc_2p5 = sensirion_common_bytes_to_float(&buffer_ptr[24]);
    *nc_4p0 = sensirion_common_bytes_to_float(&buffer_ptr[28]);
    *nc_10p0 = sensirion_common_bytes_to_float(&buffer_ptr[32]);
    *typical_particle_size = sensirion_common_bytes_to_float(&buffer_ptr[36]);
    return local_error;
}

int16_t sps30_sleep() {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x10, SPS30_SHDLC_ADDR,
                                 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    return local_error;
}

int16_t sps30_wake_up_communication() {
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0xff, SPS30_SHDLC_ADDR,
                                 0);
    sensirion_shdlc_write_request(&stream);
    return local_error;
}

int16_t sps30_wake_up() {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x11, SPS30_SHDLC_ADDR,
                                 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    /* If the device replies with execution error 0x43 (command not allowed in
       current state) for the wake command, it usually means the device is
       already awake/active. Treat this as success so the caller can proceed
       to start measurements. */
    if (local_error == SENSIRION_SHDLC_ERR_EXECUTION_FAILURE) {
        if (sensirion_shdlc_last_header.cmd == 0x11 &&
            (sensirion_shdlc_last_header.state & 0x7F) == 0x43) {
            local_error = NO_ERROR;
        }
    }
    return local_error;
}

int16_t sps30_start_fan_cleaning() {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x56, SPS30_SHDLC_ADDR,
                                 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    return local_error;
}

int16_t sps30_read_auto_cleaning_interval(uint32_t* auto_cleaning_interval) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x80, SPS30_SHDLC_ADDR,
                                 1);
    sensirion_add_uint8_t_argument(&stream, 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 4, &header, 50);
    *auto_cleaning_interval =
        sensirion_common_bytes_to_uint32_t(&buffer_ptr[0]);
    return local_error;
}

int16_t sps30_write_auto_cleaning_interval(uint32_t auto_cleaning_interval) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0x80, SPS30_SHDLC_ADDR,
                                 5);
    sensirion_add_uint8_t_argument(&stream, 0);
    sensirion_add_uint32_t_argument(&stream, auto_cleaning_interval);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    return local_error;
}

int16_t sps30_read_product_type(int8_t* product_type,
                                uint16_t product_type_size) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0xd0, SPS30_SHDLC_ADDR,
                                 1);
    sensirion_add_uint8_t_argument(&stream, 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 9, &header, 50);
    sensirion_common_copy_bytes(&buffer_ptr[0], (uint8_t*)product_type,
                                product_type_size);
    return local_error;
}

int16_t sps30_read_serial_number(int8_t* serial_number,
                                 uint16_t serial_number_size) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0xd0, SPS30_SHDLC_ADDR,
                                 1);
    sensirion_add_uint8_t_argument(&stream, 3);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 32, &header, 50);
    sensirion_common_copy_bytes(&buffer_ptr[0], (uint8_t*)serial_number,
                                serial_number_size);
    return local_error;
}

int16_t sps30_read_version(uint8_t* firmware_major_version,
                           uint8_t* firmware_minor_version, uint8_t* reserved1,
                           uint8_t* hardware_revision, uint8_t* reserved2,
                           uint8_t* shdlc_major_version,
                           uint8_t* shdlc_minor_version) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0xd1, SPS30_SHDLC_ADDR,
                                 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 7, &header, 50);
    *firmware_major_version = (uint8_t)buffer_ptr[0];
    *firmware_minor_version = (uint8_t)buffer_ptr[1];
    *reserved1 = (uint8_t)buffer_ptr[2];
    *hardware_revision = (uint8_t)buffer_ptr[3];
    *reserved2 = (uint8_t)buffer_ptr[4];
    *shdlc_major_version = (uint8_t)buffer_ptr[5];
    *shdlc_minor_version = (uint8_t)buffer_ptr[6];
    return local_error;
}

int16_t sps30_read_device_status_register(bool clear_status_register,
                                          uint32_t* device_status_register,
                                          uint8_t* reserved) {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0xd2, SPS30_SHDLC_ADDR,
                                 1);
    sensirion_add_bool_argument(&stream, clear_status_register);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 5, &header, 50);
    if (local_error) {
        return local_error;
    }
    if (device_status_register) {
        *device_status_register =
            sensirion_common_bytes_to_uint32_t(&buffer_ptr[0]);
    }
    if (reserved) {
        *reserved = (uint8_t)buffer_ptr[4];
    }
    return local_error;
}

int16_t sps30_device_reset() {
    struct sensirion_shdlc_rx_header header;
    sensirion_streaming_state stream;
    int16_t local_error = NO_ERROR;
    uint8_t* buffer_ptr = communication_buffer;
    sensirion_shdlc_begin_stream(&stream, buffer_ptr, 0xd3, SPS30_SHDLC_ADDR,
                                 0);
    local_error = sensirion_shdlc_write_request(&stream);
    if (local_error) {
        return local_error;
    }
    local_error = sensirion_shdlc_read_response(&stream, 0, &header, 50);
    return local_error;
}
