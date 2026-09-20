// fwimage.c — firmware image creation and validation.
#include "fwimage.h"
#include "crc32.h"
#include <stdio.h>
#include <string.h>

// xorshift64* — deterministic payload filler (no CRT rand dependency).
static uint32_t xorshift32(uint32_t* s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

FwImageError fw_image_create(const wchar_t* path)
{
    static const FwSegment layout[] = {
        { 0x0000, 0x1000, SEG_FLAG_BOOT,     0x0000 },  // boot: 4 KB
        { 0x1000, 0x8000, 0,                 0x1000 },  // main: 32 KB
        { 0xE000, 0x0100, SEG_FLAG_READONLY, 0x9000 },  // config: 256 B
    };
    const uint32_t nsegs = (uint32_t)(sizeof(layout) / sizeof(layout[0]));
    const uint32_t payload_size = layout[nsegs - 1].offset + layout[nsegs - 1].size;

    FwHeader hdr = { 0 };
    memcpy(hdr.magic, FW_MAGIC, 4);
    hdr.version = FW_VERSION;
    hdr.header_size = sizeof(FwHeader);
    hdr.segment_count = nsegs;
    hdr.payload_size = payload_size;

    uint8_t* payload = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, payload_size);
    if (!payload)
        return FWIMG_ERR_CREATE;

    uint32_t state = 0xC0FFEE42u;  // deterministic seed per segment index
    for (uint32_t i = 0; i < nsegs; i++) {
        state = 0xC0FFEE42u ^ (i * 0x9E3779B9u);
        uint8_t* seg = payload + layout[i].offset;
        for (uint32_t j = 0; j < layout[i].size; j += 4) {
            uint32_t r = xorshift32(&state);
            memcpy(seg + j, &r, (layout[i].size - j < 4) ? layout[i].size - j : 4);
        }
    }

    crc32_ctx crc;
    crc32_init(&crc);
    crc32_update(&crc, layout, sizeof(layout));
    crc32_update(&crc, payload, payload_size);
    hdr.image_crc = crc32_final(&crc);

    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, payload);
        return FWIMG_ERR_CREATE;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(h, &hdr, sizeof(hdr), &written, NULL)
           && written == sizeof(hdr)
           && WriteFile(h, layout, sizeof(layout), &written, NULL)
           && written == sizeof(layout)
           && WriteFile(h, payload, payload_size, &written, NULL)
           && written == payload_size;
    CloseHandle(h);
    HeapFree(GetProcessHeap(), 0, payload);
    return ok ? FWIMG_OK : FWIMG_ERR_WRITE;
}

FwImageError fw_image_validate(const void* mapped, DWORD size,
                               const FwHeader** hdr, const FwSegment** segs,
                               const uint8_t** payload)
{
    if (size < sizeof(FwHeader))
        return FWIMG_ERR_BAD_LAYOUT;
    const FwHeader* h = (const FwHeader*)mapped;
    if (memcmp(h->magic, FW_MAGIC, 4) != 0)
        return FWIMG_ERR_BAD_MAGIC;
    if (h->version != FW_VERSION)
        return FWIMG_ERR_BAD_VERSION;
    if (h->header_size != sizeof(FwHeader) || h->segment_count == 0
        || h->segment_count > FW_MAX_SEGMENTS)
        return FWIMG_ERR_BAD_LAYOUT;

    DWORD table_bytes = h->segment_count * sizeof(FwSegment);
    if ((uint64_t)sizeof(FwHeader) + table_bytes + h->payload_size != size)
        return FWIMG_ERR_BAD_LAYOUT;

    const FwSegment* s = (const FwSegment*)((const uint8_t*)mapped + h->header_size);
    const uint8_t* p = (const uint8_t*)s + table_bytes;
    for (uint32_t i = 0; i < h->segment_count; i++) {
        if ((uint64_t)s[i].offset + s[i].size > h->payload_size)
            return FWIMG_ERR_BAD_LAYOUT;
    }

    crc32_ctx crc;
    crc32_init(&crc);
    crc32_update(&crc, s, table_bytes);
    crc32_update(&crc, p, h->payload_size);
    if (crc32_final(&crc) != h->image_crc)
        return FWIMG_ERR_BAD_CRC;

    *hdr = h;
    *segs = s;
    *payload = p;
    return FWIMG_OK;
}

const wchar_t* fw_image_error_string(FwImageError e)
{
    switch (e) {
    case FWIMG_OK:           return L"ok";
    case FWIMG_ERR_CREATE:   return L"cannot create image file";
    case FWIMG_ERR_WRITE:    return L"short write while creating image";
    case FWIMG_ERR_READ:     return L"cannot read image";
    case FWIMG_ERR_BAD_MAGIC:    return L"bad magic (not a FWFL image)";
    case FWIMG_ERR_BAD_VERSION:  return L"unsupported image version";
    case FWIMG_ERR_BAD_LAYOUT:   return L"corrupt segment table / layout";
    case FWIMG_ERR_BAD_CRC:      return L"CRC32 mismatch — image corrupted";
    }
    return L"unknown error";
}
