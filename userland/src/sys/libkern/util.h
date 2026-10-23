#ifndef LIBKERN_UTIL_H_
#define LIBKERN_UTIL_H_

#include <stddef.h>
#include <stdint.h>

extern const uint32_t crc32_tab[];

uint32_t crc32(const void *buf, size_t size);
uint32_t crc32_ext(const void *buf, size_t size, uint32_t ocrc);
uint32_t iscsi_crc32(const void *buf, size_t size);
uint32_t iscsi_crc32_ext(const void *buf, size_t size, uint32_t ocrc);
uint32_t calculate_crc32c(uint32_t crc32c, const unsigned char *buffer,
			unsigned int length);

#endif
