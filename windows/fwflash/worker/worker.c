// worker.c — worker.exe: child process spawned by fwflash to independently
// re-verify the flashed device backing file.
//
//   worker.exe --verify <file> <hexcrc> <size>
//
// Maps the file read-only, CRC32s the first <size> bytes, exits 0 on match.
#include "../src/crc32.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

int main(int argc, char** argv)
{
    if (argc != 5)
        return 2;

    wchar_t file[MAX_PATH];
    if (MultiByteToWideChar(CP_UTF8, 0, argv[2], -1, file, MAX_PATH) == 0)
        return 2;
    uint32_t expected = (uint32_t)strtoul(argv[3], NULL, 16);
    uint32_t size = (uint32_t)strtoul(argv[4], NULL, 10);

    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("[worker] cannot open %ls (error %lu)\n", file, GetLastError());
        return 1;
    }

    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const void* view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!view) {
        printf("[worker] cannot map %ls\n", file);
        CloseHandle(h);
        return 1;
    }

    crc32_ctx crc;
    crc32_init(&crc);
    crc32_update(&crc, view, size);
    uint32_t actual = crc32_final(&crc);

    UnmapViewOfFile(view);
    CloseHandle(map);
    CloseHandle(h);

    if (actual == expected) {
        printf("[worker] verify OK: crc32=%08lx over %lu bytes\n",
               (unsigned long)actual, (unsigned long)size);
        return 0;
    }
    printf("[worker] verify FAILED: crc32=%08lx, expected %08lx\n",
           (unsigned long)actual, (unsigned long)expected);
    return 1;
}
