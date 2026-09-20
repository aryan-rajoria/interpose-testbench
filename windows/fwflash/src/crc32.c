// crc32.c — reflected CRC-32, table built at runtime (keeps the binary
// dependency-free and the algorithm visible).
#include "crc32.h"

void crc32_init(crc32_ctx* ctx)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        ctx->table[i] = c;
    }
    ctx->state = 0xFFFFFFFFu;
}

void crc32_update(crc32_ctx* ctx, const void* data, uint32_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = ctx->state;
    for (uint32_t i = 0; i < size; i++)
        c = ctx->table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    ctx->state = c;
}

uint32_t crc32_final(const crc32_ctx* ctx)
{
    return ctx->state ^ 0xFFFFFFFFu;
}
