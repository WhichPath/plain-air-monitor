/* sensirion_common.h - helper functions */

#ifndef SENSIRION_COMMON_H
#define SENSIRION_COMMON_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t sensirion_common_bytes_to_uint16_t(const uint8_t* bytes);
uint32_t sensirion_common_bytes_to_uint32_t(const uint8_t* bytes);
float sensirion_common_bytes_to_float(const uint8_t* bytes);
void sensirion_common_copy_bytes(const uint8_t* src, uint8_t* dst, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
