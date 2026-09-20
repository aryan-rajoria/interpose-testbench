// fwimage.h — firmware image format for the fwflash simulator.
//
//   layout:  [ FwHeader ][ FwSegment × N ][ payload bytes ]
//   CRC32 in the header covers the segment table + payload (everything
//   after the crc field itself).
#ifndef FWFLASH_FWIMAGE_H
#define FWFLASH_FWIMAGE_H

#include <stdint.h>
#include <windows.h>

#define FW_MAGIC            "FWFL"
#define FW_VERSION          0x00010000u   // 1.0
#define FW_MAX_SEGMENTS     16
#define SEG_FLAG_BOOT       0x00000001u
#define SEG_FLAG_READONLY   0x00000002u

#pragma pack(push, 1)
typedef struct {
    char     magic[4];        // FW_MAGIC
    uint32_t version;         // FW_VERSION
    uint32_t header_size;     // sizeof(FwHeader)
    uint32_t segment_count;
    uint32_t payload_size;    // total bytes of payload region
    uint32_t image_crc;       // CRC32 of segment table + payload
} FwHeader;

typedef struct {
    uint32_t address;         // flash target address
    uint32_t size;            // bytes
    uint32_t flags;           // SEG_FLAG_*
    uint32_t offset;          // offset into payload region
} FwSegment;
#pragma pack(pop)

typedef enum {
    FWIMG_OK = 0,
    FWIMG_ERR_CREATE,
    FWIMG_ERR_WRITE,
    FWIMG_ERR_READ,
    FWIMG_ERR_BAD_MAGIC,
    FWIMG_ERR_BAD_VERSION,
    FWIMG_ERR_BAD_LAYOUT,
    FWIMG_ERR_BAD_CRC
} FwImageError;

// Build a deterministic image (3 segments: boot / main / config) and write it.
FwImageError fw_image_create(const wchar_t* path);

// Validate a mapped image: bounds, magic, version, CRC. On success the out
// pointers alias into the mapping.
FwImageError fw_image_validate(const void* mapped, DWORD size,
                               const FwHeader** hdr, const FwSegment** segs,
                               const uint8_t** payload);

const wchar_t* fw_image_error_string(FwImageError e);

#endif
