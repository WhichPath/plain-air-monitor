/*
 * Copyright (c) 2018, Sensirion AG
 * All rights reserved.
 */

#ifndef SENSIRION_SHDLC_H
#define SENSIRION_SHDLC_H

#include "sensirion_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSIRION_SHDLC_ERR_NO_DATA -1
#define SENSIRION_SHDLC_ERR_MISSING_START -2
#define SENSIRION_SHDLC_ERR_MISSING_STOP -3
#define SENSIRION_SHDLC_ERR_CRC_MISMATCH -4
#define SENSIRION_SHDLC_ERR_ENCODING_ERROR -5
#define SENSIRION_SHDLC_ERR_TX_INCOMPLETE -6
#define SENSIRION_SHDLC_ERR_FRAME_TOO_LONG -7
#define SENSIRION_SHDLC_ERR_EXECUTION_FAILURE -8

struct sensirion_shdlc_buffer {
	uint8_t* data;
	uint16_t offset;
	uint8_t checksum;
};

struct sensirion_shdlc_rx_header {
	uint8_t addr;
	uint8_t cmd;
	uint8_t state;
	uint8_t data_len;
};

/**
 * sensirion_shdlc_tx() - transmit an SHDLC frame
 *
 * @addr:       SHDLC recipient address
 * @cmd:        command parameter
 * @data_len:   data length to send
 * @data:       data to send
 * Return:      0 on success, an error code otherwise
 */
int16_t sensirion_shdlc_tx(uint8_t addr, uint8_t cmd, uint8_t data_len,
						   const uint8_t* data);

/**
 * sensirion_shdlc_rx() - receive an SHDLC frame
 *
 * Note that the header and data must be discarded on failure
 *
 * @data_len:   max data length to receive
 * @header:     Memory where the SHDLC header containing the sender address,
 *              command, sensor state and data length is stored
 * @data:       Memory where received data is stored
 * Return:      0 on success, an error code otherwise
 */
int16_t sensirion_shdlc_rx(uint8_t max_data_len,
						   struct sensirion_shdlc_rx_header* header,
						   uint8_t* data);

/**
 * sensirion_shdlc_xcv() - transceive (transmit then receive) an SHDLC frame
 *
 * Note that rx_header and rx_data must be discarded on failure
 *
 * @addr:           recipient address
 * @cmd:            parameter
 * @tx_data_len:    data length to send
 * @tx_data:        data to send
 * @rx_header:      Memory where the SHDLC header containing the sender address,
 *                  command, state and data_length is stored
 * @rx_data:        Memory where the received data is stored
 * Return:          0 on success, an error code otherwise
 */
int16_t sensirion_shdlc_xcv(uint8_t addr, uint8_t cmd, uint8_t tx_data_len,
							const uint8_t* tx_data, uint8_t max_rx_data_len,
							struct sensirion_shdlc_rx_header* rx_header,
							uint8_t* rx_data);

void sensirion_shdlc_add_uint8_t_to_frame(
	struct sensirion_shdlc_buffer* tx_frame, uint8_t data);

void sensirion_shdlc_begin_frame(struct sensirion_shdlc_buffer* tx_frame,
								 uint8_t* buffer, uint8_t command,
								 uint8_t address, uint8_t data_length);

void sensirion_shdlc_add_bool_to_frame(struct sensirion_shdlc_buffer* tx_frame,
									   bool data);

void sensirion_shdlc_add_uint32_t_to_frame(
	struct sensirion_shdlc_buffer* tx_frame, uint32_t data);

void sensirion_shdlc_add_int32_t_to_frame(
	struct sensirion_shdlc_buffer* tx_frame, int32_t data);

void sensirion_shdlc_add_uint16_t_to_frame(
	struct sensirion_shdlc_buffer* tx_frame, uint16_t data);

void sensirion_shdlc_add_int16_t_to_frame(
	struct sensirion_shdlc_buffer* tx_frame, int16_t data);

void sensirion_shdlc_add_float_to_frame(struct sensirion_shdlc_buffer* tx_frame,
										float data);

void sensirion_shdlc_add_bytes_to_frame(struct sensirion_shdlc_buffer* tx_frame,
										const uint8_t* data,
										uint16_t data_length);

void sensirion_shdlc_finish_frame(struct sensirion_shdlc_buffer* tx_frame);

int16_t sensirion_shdlc_tx_frame(struct sensirion_shdlc_buffer* tx_frame);

int16_t sensirion_shdlc_rx_inplace(struct sensirion_shdlc_buffer* rx_frame,
								   uint8_t expected_data_length,
								   struct sensirion_shdlc_rx_header* header);

#ifdef __cplusplus
}
#endif

#endif /* SENSIRION_SHDLC_H */
