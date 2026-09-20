// device.h — plugin contract shared between fwflash.exe and device.dll.
//
// device.dll is loaded at runtime with LoadLibraryExW, so it never appears
// in fwflash.exe's static import table — exactly the kind of dependency a
// static tool (dumpbin /dependents) misses and the depcheck interposer sees.
#ifndef FWFLASH_DEVICE_H
#define FWFLASH_DEVICE_H

#include <windows.h>

#define DEVICE_SIZE  0x10000u   // 64 KB simulated NOR flash

typedef void* DEVICE_HANDLE;

typedef struct {
    // Open (creating + erasing) a backing file as the simulated device.
    DEVICE_HANDLE (WINAPI *Open)(const wchar_t* backing_path);
    // Erase sets the range to 0xFF (NOR erased state).
    BOOL (WINAPI *Erase)(DEVICE_HANDLE h, DWORD address, DWORD size);
    // NOR program: bits can only go 1 -> 0; violating writes fail.
    BOOL (WINAPI *Write)(DEVICE_HANDLE h, DWORD address,
                         const void* data, DWORD size);
    BOOL (WINAPI *Read)(DEVICE_HANDLE h, DWORD address,
                        void* buffer, DWORD size);
    void (WINAPI *Close)(DEVICE_HANDLE h);
} DEVICE_VTABLE;

// The single export of device.dll.
__declspec(dllexport) const DEVICE_VTABLE* WINAPI DeviceGetVTable(void);

#endif
