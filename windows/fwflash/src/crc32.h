// crc32.h — hand-rolled reflected CRC-32 (poly 0xEDB88320), streaming API.
#ifndef FWFLASH_CRC32_H
#define FWFLASH_CRC32_H

#include <stdint.h>

typedef struct {
    uint32_t state;
    uint32_t table[256];
} crc32_ctx;

void     crc32_init(crc32_ctx* ctx);
void     crc32_update(crc32_ctx* ctx, const void* data, uint32_t size);
uint32_t crc32_final(const crc32_ctx* ctx);

#endif
