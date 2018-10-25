#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>

typedef struct __attribute__((packed)) {
	uint64_t index;
	uint64_t capture_ts;
	uint64_t encode_ts;
	uint32_t size;
	int32_t codec;
	int32_t format;
	uint16_t width;
	uint16_t height;
	uint16_t framerate;
} S_Header;


#endif /* COMMON_H */
