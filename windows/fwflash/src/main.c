// main.c — fwflash.exe: low-level firmware flasher simulator (Win32 C).
//
//   fwflash.exe --create <image.bin>   build a deterministic FWFL image
//   fwflash.exe --flash <image.bin>    validate + flash it through device.dll
//
// The --flash path deliberately exercises everything an interposer sees:
//   CreateFileW / CreateFileMappingW / MapViewOfFile   (image reads)
//   LoadLibraryExW + GetProcAddress                    (runtime-only plugin)
//   BCrypt* via /DELAYLOAD:bcrypt.dll                  (delay-loaded import)
//   CreateProcessW                                     (worker.exe child)
#include <windows.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include "fwimage.h"
#include "crc32.h"
#include "../device/device.h"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "delayimp.lib")

static void print_header(const FwHeader* h)
{
    printf("[fwflash] image version %u.%u, %u segment(s), %u payload bytes, crc32=%08lx\n",
           h->version >> 16, h->version & 0xFFFF,
           h->segment_count, h->payload_size, (unsigned long)h->image_crc);
}

static void print_segments(const FwHeader* h, const FwSegment* segs)
{
    for (uint32_t i = 0; i < h->segment_count; i++) {
        printf("[fwflash]   seg %u: addr=0x%04x size=0x%04x flags=%s%s%s\n",
               i, segs[i].address, segs[i].size,
               (segs[i].flags & SEG_FLAG_BOOT) ? "BOOT " : "",
               (segs[i].flags & SEG_FLAG_READONLY) ? "RO " : "",
               (!(segs[i].flags & (SEG_FLAG_BOOT | SEG_FLAG_READONLY))) ? "data" : "");
    }
}

// SHA-256 of the payload via BCrypt — this is the delay-load trigger:
// bcrypt.dll is listed as a *delay* import, so it only loads when this runs.
static void print_sha256(const uint8_t* payload, uint32_t size)
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE digest[32];
    DWORD done = 0;

    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))
        || !BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0))
        || !BCRYPT_SUCCESS(BCryptHashData(hash, (PUCHAR)payload, size, 0))
        || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) {
        printf("[fwflash] sha256: unavailable\n");
    } else {
        printf("[fwflash] payload sha256: ");
        for (int i = 0; i < 32; i++)
            printf("%02x", digest[i]);
        printf("\n");
    }

    if (hash) BCryptDestroyHash(hash);
    if (alg)  BCryptCloseAlgorithmProvider(alg, 0);
    (void)done;
}

static int cmd_create(const wchar_t* path)
{
    FwImageError e = fw_image_create(path);
    if (e != FWIMG_OK) {
        printf("[fwflash] create failed: %ls\n", fw_image_error_string(e));
        return 1;
    }
    printf("[fwflash] wrote %ls\n", path);
    return 0;
}

static BOOL path_next_to_exe(wchar_t* out, size_t cch, const wchar_t* name)
{
    DWORD n = GetModuleFileNameW(NULL, out, (DWORD)cch);
    if (n == 0 || n >= cch)
        return FALSE;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash)
        return FALSE;
    wcscpy(slash + 1, name);
    return TRUE;
}

static int cmd_flash(const wchar_t* image_path)
{
    // 1. map the image read-only and validate it
    HANDLE h = CreateFileW(image_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        printf("[fwflash] cannot open %ls (error %lu)\n", image_path, GetLastError());
        return 1;
    }
    DWORD size = GetFileSize(h, NULL);
    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    const void* view = map ? MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0) : NULL;
    if (!view) {
        printf("[fwflash] cannot map %ls\n", image_path);
        CloseHandle(h);
        return 1;
    }

    const FwHeader* hdr;
    const FwSegment* segs;
    const uint8_t* payload;
    FwImageError e = fw_image_validate(view, size, &hdr, &segs, &payload);
    if (e != FWIMG_OK) {
        printf("[fwflash] image rejected: %ls\n", fw_image_error_string(e));
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    print_header(hdr);
    print_segments(hdr, segs);
    print_sha256(payload, hdr->payload_size);

    // 2. load the device plugin at runtime (invisible to static imports)
    HMODULE devmod = LoadLibraryExW(L"device.dll", NULL, 0);
    if (!devmod) {
        printf("[fwflash] LoadLibraryExW(device.dll) failed (error %lu)\n",
               GetLastError());
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    const DEVICE_VTABLE* (*getvt)(void) =
        (const DEVICE_VTABLE* (WINAPI*)(void))GetProcAddress(devmod, "DeviceGetVTable");
    if (!getvt) {
        printf("[fwflash] device.dll has no DeviceGetVTable export\n");
        FreeLibrary(devmod);
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    const DEVICE_VTABLE* dev = getvt();

    // 3. open the simulated device (backing file next to this exe)
    wchar_t backing[MAX_PATH * 2];
    if (!path_next_to_exe(backing, MAX_PATH * 2, L"device.bin")) {
        FreeLibrary(devmod);
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    DEVICE_HANDLE dh = dev->Open(backing);
    if (!dh) {
        printf("[fwflash] device open failed (%ls)\n", backing);
        FreeLibrary(devmod);
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    printf("[fwflash] device opened: %ls (%u bytes NOR)\n", backing, DEVICE_SIZE);

    // 4. erase + program each segment
    for (uint32_t i = 0; i < hdr->segment_count; i++) {
        if (!dev->Erase(dh, segs[i].address, segs[i].size)
            || !dev->Write(dh, segs[i].address, payload + segs[i].offset,
                           segs[i].size)) {
            printf("[fwflash] flash of segment %u failed\n", i);
            dev->Close(dh);
            FreeLibrary(devmod);
            UnmapViewOfFile(view);
            CloseHandle(map);
            CloseHandle(h);
            return 1;
        }
        printf("[fwflash] flashed seg %u -> 0x%04x (0x%x bytes)\n",
               i, segs[i].address, segs[i].size);
    }

    // spot-check a read-back of the first 16 bytes of segment 0
    BYTE check[16];
    if (dev->Read(dh, segs[0].address, check, sizeof(check))
        && memcmp(check, payload + segs[0].offset, sizeof(check)) == 0) {
        printf("[fwflash] read-back spot check OK\n");
    }

    // 5. compute the expected device CRC over the used extent, then spawn
    //    worker.exe to independently re-verify it
    DWORD extent = 0;
    for (uint32_t i = 0; i < hdr->segment_count; i++) {
        DWORD end = segs[i].address + segs[i].size;
        if (end > extent) extent = end;
    }
    BYTE* devicebuf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, extent);
    uint32_t device_crc = 0;
    if (devicebuf && dev->Read(dh, 0, devicebuf, extent)) {
        crc32_ctx crc;
        crc32_init(&crc);
        crc32_update(&crc, devicebuf, extent);
        device_crc = crc32_final(&crc);
    }
    HeapFree(GetProcessHeap(), 0, devicebuf);
    dev->Close(dh);
    FreeLibrary(devmod);

    wchar_t worker[MAX_PATH * 2];
    if (!path_next_to_exe(worker, MAX_PATH * 2, L"worker.exe")) {
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    wchar_t cmd[2048];
    _snwprintf(cmd, 2047, L"\"%ls\" --verify \"%ls\" %08lx %lu",
               worker, backing, (unsigned long)device_crc, (unsigned long)extent);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("[fwflash] spawning worker failed (error %lu)\n", GetLastError());
        UnmapViewOfFile(view);
        CloseHandle(map);
        CloseHandle(h);
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    UnmapViewOfFile(view);
    CloseHandle(map);
    CloseHandle(h);

    printf("[fwflash] flash %s (worker exit=%lu)\n",
           code == 0 ? "complete" : "FAILED", (unsigned long)code);
    return (int)code;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && wcscmp(argv[1], L"--create") == 0)
        return cmd_create(argv[2]);
    if (argc == 3 && wcscmp(argv[1], L"--flash") == 0)
        return cmd_flash(argv[2]);

    printf("usage: fwflash --create <image.bin> | --flash <image.bin>\n");
    return 2;
}
