/*
 * Copyright (c) 2024, Sensirion AG
 * All rights reserved.
 */

/**
 *  @file sensirion_streaming_shdlc.h
 */
#ifndef SENSIRION_STREAMING_SHDLC_H
#define SENSIRION_STREAMING_SHDLC_H

#include "sensirion_common.h"
#include "sensirion_shdlc.h"
#include "sensirion_streaming.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * sensirion_shdlc_begin_stream() - Initialize buffer and add the first three
 *                                  fixed-use data bytes to it.
 *
 * @param stream Data structure that holds the state while data is
 *                     received or transmitted.
 * @param buffer   The pointer to big enough buffer to hold the payload that
 *                 needs to be sent (+ 4 bytes for an possible protocol header).
 * @param command     Command to be written into the stream.
 * @param address     Address to be written into the stream.
 * @param data_length Specifies the size of the parameters.
 */
void sensirion_shdlc_begin_stream(sensirion_streaming_state* stream,
                                  uint8_t* buffer, uint8_t command,
                                  uint8_t address, uint8_t data_length);

/**
 * sensirion_shdlc_write_request() - Transmit the SHDLC request.
 *
 * @param stream Data structure that holds the state while data is
 *                     received or transmitted.
 * @return         NO_ERROR on success, an error code otherwise.
 */
int16_t sensirion_shdlc_write_request(sensirion_streaming_state* stream);

/**
 * sensirion_shdlc_read_response() - Receive data from the slave.
 *
 * @note The header and data must be discarded on failure
 *
 * @param stream Data structure that holds the state while data is
 *                     received or transmitted.
 * @param expected_data_length Expected data amount to receive.
 * @param header               Memory where the SHDLC header containing the
 *                             sender address, command, state and data_length
 *                             is stored.
 * @param max_timeout_ms       timeout in milliseconds. This is the maximum time
 * the request is allowed to take.
 *
 * @return            NO_ERROR on success, an error code otherwise
 */
int16_t sensirion_shdlc_read_response(sensirion_streaming_state* stream,
                                      uint8_t expected_data_length,
                                      struct sensirion_shdlc_rx_header* header,
                                      uint32_t max_timeout_ms);

/* Last received/frame debugging info (updated on errors) */
extern uint8_t sensirion_shdlc_last_first_byte;
extern struct sensirion_shdlc_rx_header sensirion_shdlc_last_header;

#ifdef __cplusplus
}
#endif

#endif  // SENSIRION_STREAMING_SHDLC_H
