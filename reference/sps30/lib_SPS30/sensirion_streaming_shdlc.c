/*
 * Copyright (c) 2024, Sensirion AG
 * All rights reserved.
 */

/**
 *  @file sensirion_streaming_shdlc.c
 */
#include "sensirion_streaming_shdlc.h"
#include "sensirion_streaming.h"
#include "sensirion_uart_hal.h"

/* Debug globals: kept small and C-compatible so application code can read them
    when SHDLC errors occur. Updated before returning from error paths. */
uint8_t sensirion_shdlc_last_first_byte = 0;
struct sensirion_shdlc_rx_header sensirion_shdlc_last_header = {0,0,0,0};

#define SHDLC_FRAME_DELIMITER 0x7e
#define SHDLC_STUFF_BYTE 0x7d

#define SHDLC_MOSI_ADDR_POS 0
#define SHDLC_MOSI_CMD_POS 1
#define SHDLC_MOSI_LEN_POS 2

static void sensirion_shdlc_stream_stuff_and_write_next_byte(
    sensirion_streaming_state* stream, uint8_t byte) {
    stream->checksum += byte;
    switch (byte) {
        case 0x11:
        case 0x13:
        case SHDLC_STUFF_BYTE:
        case SHDLC_FRAME_DELIMITER: {
            /* byte stuffing is done by inserting 0x7d and inverting bit 5
             */
            uint8_t stuffed = SHDLC_STUFF_BYTE;
            stream->stream_status = stream->stream.write(1, &stuffed);
            if (stream->stream_status != 1) {
                return;
            }
            byte = byte ^ (1 << 5);
            break;
        }
        default:
            break;
    }
    stream->stream_status = stream->stream.write(1, &byte);
}

static uint8_t sensirion_shdlc_stream_read_and_unstuff_next_byte(
    sensirion_streaming_state* stream) {

    uint8_t data = 0;
    stream->stream_status = stream->stream.read(1, &data);
    if (stream->stream_status < 0) {
        return 0;
    }
    if (data == SHDLC_STUFF_BYTE) {
        stream->stream_status = stream->stream.read(1, &data);
        if (stream->stream_status < 0) {
            return 0;
        }
        data = data ^ (1 << 5);
    }
    stream->checksum += data;
    return data;
}

void sensirion_shdlc_begin_stream(sensirion_streaming_state* stream,
                                  uint8_t* buffer, uint8_t command,
                                  uint8_t address, uint8_t data_length) {
    stream->data = buffer;
    stream->checksum = 0;
    stream->stream_status = 0;
    stream->data[SHDLC_MOSI_ADDR_POS] = address;
    stream->data[SHDLC_MOSI_CMD_POS] = command;
    stream->data[SHDLC_MOSI_LEN_POS] = data_length;
    stream->offset = SHDLC_MOSI_LEN_POS + 1;
}

int16_t sensirion_shdlc_write_request(sensirion_streaming_state* stream) {
    stream->stream.write = sensirion_uart_hal_tx;
    uint8_t shdlc_frame_delimiter = SHDLC_FRAME_DELIMITER;
    if ((stream->offset - 3) != stream->data[SHDLC_MOSI_LEN_POS]) {
        return SENSIRION_SHDLC_ERR_ENCODING_ERROR;
    }
    stream->stream_status = stream->stream.write(1, &shdlc_frame_delimiter);
    if (stream->stream_status != 1) {
        return SENSIRION_SHDLC_ERR_TX_INCOMPLETE;
    }
    for (uint16_t i = 0; i < stream->offset; i++) {
        sensirion_shdlc_stream_stuff_and_write_next_byte(stream,
                                                         stream->data[i]);
        if (stream->stream_status != 1) {
            return SENSIRION_SHDLC_ERR_TX_INCOMPLETE;
        }
    }
    sensirion_shdlc_stream_stuff_and_write_next_byte(stream,
                                                     ~(stream->checksum));
    if (stream->stream_status != 1) {
        return SENSIRION_SHDLC_ERR_TX_INCOMPLETE;
    }
    stream->stream_status = stream->stream.write(1, &shdlc_frame_delimiter);
    if (stream->stream_status != 1) {
        return SENSIRION_SHDLC_ERR_TX_INCOMPLETE;
    }
    return NO_ERROR;
}

int16_t sensirion_shdlc_read_response(sensirion_streaming_state* stream,
                                      uint8_t expected_data_length,
                                      struct sensirion_shdlc_rx_header* header,
                                      uint32_t max_timeout_ms) {
    stream->offset = 0;
    stream->checksum = 0;
    stream->stream_status = 0;
    stream->stream.read = sensirion_uart_hal_rx;
    uint8_t data = 0;
    uint32_t retries = max_timeout_ms;

    // Poll for data available
    while ((stream->stream.read(1, &data) <= 0) && (retries-- > 0)) {
        sensirion_uart_hal_sleep_usec(1000);
    }

    // read the beginning of the frame
    if (data != SHDLC_FRAME_DELIMITER) {
        sensirion_shdlc_last_first_byte = data;
        return SENSIRION_SHDLC_ERR_MISSING_START;
    }
    // read the header
    uint8_t* ptr = (uint8_t*)header;
    for (uint8_t i = 0; i < sizeof(struct sensirion_shdlc_rx_header); i++) {
        *ptr = sensirion_shdlc_stream_read_and_unstuff_next_byte(stream);
        if (stream->stream_status < 0) {
            return SENSIRION_SHDLC_ERR_MISSING_STOP;
        }
        ptr++;
    }
    // consistency check with data read from header
    if (expected_data_length < header->data_len) {
        sensirion_shdlc_last_header = *header;
        return SENSIRION_SHDLC_ERR_FRAME_TOO_LONG;
    }
    // read all data
    while ((stream->offset < header->data_len) &&
           (stream->stream_status >= 0)) {
        data = sensirion_shdlc_stream_read_and_unstuff_next_byte(stream);
        if (stream->stream_status < 0) {
            return SENSIRION_SHDLC_ERR_MISSING_STOP;
        }
        if (stream->stream_status == 0) {
            if (retries == 0) {
                return SENSIRION_SHDLC_ERR_MISSING_STOP;
            }
            retries--;
            sensirion_uart_hal_sleep_usec(1000);
            continue;
        }
        stream->data[stream->offset++] = data;
    }

    if (stream->offset < header->data_len) {
        sensirion_shdlc_last_header = *header;
        return SENSIRION_SHDLC_ERR_ENCODING_ERROR;
    }

    // read checksum, the data byte is not needed as the checksum
    // is computed behind the scene
    sensirion_shdlc_stream_read_and_unstuff_next_byte(stream);
    /* (CHECKSUM + ~CHECKSUM) = 0xFF */
    if (stream->checksum != 0xFF) {
        sensirion_shdlc_last_header = *header;
        return SENSIRION_SHDLC_ERR_CRC_MISMATCH;
    }

    // read expected frame delimiter before checking state
    stream->stream.read(1, &data);

    if (0x7F & header->state) {
        sensirion_shdlc_last_header = *header;
        return SENSIRION_SHDLC_ERR_EXECUTION_FAILURE;
    }

    if (data != SHDLC_FRAME_DELIMITER) {
        return SENSIRION_SHDLC_ERR_MISSING_STOP;
    }

    return NO_ERROR;
}
