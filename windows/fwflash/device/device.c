// device.c — device.dll: a simulated NOR flash device backed by a file.
// All I/O goes through raw Win32 (CreateFileW / SetFilePointer / Read-WriteFile).
#include "device.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    HANDLE file;
} DEVICE;

static DWORD seek_read(DEVICE* d, DWORD address, void* buffer, DWORD size)
{
    if (SetFilePointer(d->file, (LONG)address, NULL, FILE_BEGIN)
        == INVALID_SET_FILE_POINTER)
        return 0;
    DWORD got = 0;
    if (!ReadFile(d->file, buffer, size, &got, NULL))
        return 0;
    return got;
}

static DEVICE_HANDLE WINAPI dev_Open(const wchar_t* backing_path)
{
    DEVICE* d = (DEVICE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d)
        return NULL;

    d->file = CreateFileW(backing_path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (d->file == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    // Power-on erase: fill the whole device with 0xFF.
    BYTE ff[4096];
    memset(ff, 0xFF, sizeof(ff));
    for (DWORD off = 0; off < DEVICE_SIZE; off += sizeof(ff)) {
        DWORD chunk = DEVICE_SIZE - off;
        if (chunk > sizeof(ff)) chunk = sizeof(ff);
        DWORD written = 0;
        if (!WriteFile(d->file, ff, chunk, &written, NULL) || written != chunk) {
            CloseHandle(d->file);
            HeapFree(GetProcessHeap(), 0, d);
            return NULL;
        }
    }
    return (DEVICE_HANDLE)d;
}

static BOOL WINAPI dev_Erase(DEVICE_HANDLE h, DWORD address, DWORD size)
{
    DEVICE* d = (DEVICE*)h;
    if (!d || address + size > DEVICE_SIZE)
        return FALSE;

    if (SetFilePointer(d->file, (LONG)address, NULL, FILE_BEGIN)
        == INVALID_SET_FILE_POINTER)
        return FALSE;

    BYTE ff[1024];
    memset(ff, 0xFF, sizeof(ff));
    DWORD done = 0;
    while (done < size) {
        DWORD chunk = size - done;
        if (chunk > sizeof(ff)) chunk = sizeof(ff);
        DWORD written = 0;
        if (!WriteFile(d->file, ff, chunk, &written, NULL) || written != chunk)
            return FALSE;
        done += chunk;
    }
    return TRUE;
}

static BOOL WINAPI dev_Write(DEVICE_HANDLE h, DWORD address,
                             const void* data, DWORD size)
{
    DEVICE* d = (DEVICE*)h;
    if (!d || address + size > DEVICE_SIZE)
        return FALSE;

    // NOR semantics: read the current bytes, require new bits to be a subset
    // of old bits (programming can only clear bits), then write back.
    BYTE* old = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!old)
        return FALSE;
    BOOL ok = seek_read(d, address, old, size) == size;
    if (ok) {
        const BYTE* neu = (const BYTE*)data;
        for (DWORD i = 0; i < size; i++) {
            if ((old[i] & neu[i]) != neu[i]) {   // someone sets a 1-bit
                ok = FALSE;
                break;
            }
        }
    }
    if (ok) {
        if (SetFilePointer(d->file, (LONG)address, NULL, FILE_BEGIN)
            == INVALID_SET_FILE_POINTER)
            ok = FALSE;
        else {
            DWORD written = 0;
            ok = WriteFile(d->file, data, size, &written, NULL)
                 && written == size;
        }
    }
    HeapFree(GetProcessHeap(), 0, old);
    return ok;
}

static BOOL WINAPI dev_Read(DEVICE_HANDLE h, DWORD address,
                            void* buffer, DWORD size)
{
    DEVICE* d = (DEVICE*)h;
    if (!d || address + size > DEVICE_SIZE)
        return FALSE;
    return seek_read(d, address, buffer, size) == size;
}

static void WINAPI dev_Close(DEVICE_HANDLE h)
{
    DEVICE* d = (DEVICE*)h;
    if (!d)
        return;
    CloseHandle(d->file);
    HeapFree(GetProcessHeap(), 0, d);
}

static const DEVICE_VTABLE g_vtable = {
    dev_Open, dev_Erase, dev_Write, dev_Read, dev_Close
};

const DEVICE_VTABLE* WINAPI DeviceGetVTable(void)
{
    return &g_vtable;
}
