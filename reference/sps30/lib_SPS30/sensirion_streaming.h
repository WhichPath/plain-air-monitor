#ifndef SENSIRION_STREAMING_H
#define SENSIRION_STREAMING_H

#include "sensirion_config.h"
#include "sensirion_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sensirion_streaming_state_tag {
    uint8_t* data;    
    uint16_t offset;  
    uint8_t checksum;       
    int16_t stream_status;  
    union {
        int16_t (*read)(uint16_t length, uint8_t* data);
        int16_t (*write)(uint16_t length, const uint8_t* data);
    } stream;
} sensirion_streaming_state;

void sensirion_add_uint8_t_argument(sensirion_streaming_state* stream, uint8_t data);
void sensirion_add_bool_argument(sensirion_streaming_state* stream, bool data);
void sensirion_add_uint32_t_argument(sensirion_streaming_state* stream, uint32_t data);
void sensirion_add_int32_t_argument(sensirion_streaming_state* stream, int32_t data);
void sensirion_add_uint16_t_argument(sensirion_streaming_state* stream, uint16_t data);
void sensirion_add_int16_t_argument(sensirion_streaming_state* stream, int16_t data);
void sensirion_add_float_argument(sensirion_streaming_state* stream, float data);
void sensirion_add_bytes_argument(sensirion_streaming_state* stream, const uint8_t* data, uint16_t data_length);

#ifdef __cplusplus
}
#endif

#endif
