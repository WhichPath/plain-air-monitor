#include "sensirion_streaming.h"
#include <string.h>

void sensirion_add_uint8_t_argument(sensirion_streaming_state* stream, uint8_t data) {
    stream->data[stream->offset++] = data;
}

void sensirion_add_bool_argument(sensirion_streaming_state* stream, bool data) {
    stream->data[stream->offset++] = data ? 1 : 0;
}

void sensirion_add_uint32_t_argument(sensirion_streaming_state* stream, uint32_t data) {
    stream->data[stream->offset++] = (data >> 24) & 0xFF;
    stream->data[stream->offset++] = (data >> 16) & 0xFF;
    stream->data[stream->offset++] = (data >> 8) & 0xFF;
    stream->data[stream->offset++] = (data) & 0xFF;
}

void sensirion_add_int32_t_argument(sensirion_streaming_state* stream, int32_t data) {
    sensirion_add_uint32_t_argument(stream, (uint32_t)data);
}

void sensirion_add_uint16_t_argument(sensirion_streaming_state* stream, uint16_t data) {
    stream->data[stream->offset++] = (data >> 8) & 0xFF;
    stream->data[stream->offset++] = (data) & 0xFF;
}

void sensirion_add_int16_t_argument(sensirion_streaming_state* stream, int16_t data) {
    sensirion_add_uint16_t_argument(stream, (uint16_t)data);
}

void sensirion_add_float_argument(sensirion_streaming_state* stream, float data) {
    union { uint32_t i; float f; } u;
    u.f = data;
    sensirion_add_uint32_t_argument(stream, u.i);
}

void sensirion_add_bytes_argument(sensirion_streaming_state* stream, const uint8_t* data, uint16_t data_length) {
    memcpy(&stream->data[stream->offset], data, data_length);
    stream->offset += data_length;
}
