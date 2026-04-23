#include "sensirion_common.h"

uint16_t sensirion_common_bytes_to_uint16_t(const uint8_t* bytes) {
    return (uint16_t)((bytes[0] << 8) | bytes[1]);
}

uint32_t sensirion_common_bytes_to_uint32_t(const uint8_t* bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | ((uint32_t)bytes[3]);
}

float sensirion_common_bytes_to_float(const uint8_t* bytes) {
    union { uint32_t i; float f; } u;
    u.i = ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | ((uint32_t)bytes[3]);
    return u.f;
}

void sensirion_common_copy_bytes(const uint8_t* src, uint8_t* dst, uint16_t len) {
    memcpy(dst, src, len);
}
