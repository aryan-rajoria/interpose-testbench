// payload.cpp — depcheck.dll: the Detours interposer.
//
// Modeled on windows-monitoring/docs/rewriters/full-interposition/04-detours-api-hooking.md
// and Detours' own trcbld sample:
//   * CreateProcessW/A hooks log every exec AND re-launch each child through
//     DetourCreateProcessWithDllExW with our own DLL path, so the interposer
//     propagates across the whole process tree (the LD_PRELOAD equivalent).
//     The saved trampoline (Real_*) is passed as the spawn routine — passing
//     NULL would re-enter our own hook (infinite recursion).
//   * CreateFileW / LoadLibraryExW hooks log opens and runtime DLL loads.
//   * Hooks never do file I/O: events go to a ring buffer drained by a thread
//     started after attach (loader-lock rule from the doc's pitfalls).
//   * The child's architecture is checked against the DLL's before injection
//     (the doc's architecture matrix): non-AMD64 children run unhooked.
#include <windows.h>
#include <detours.h>
#include <cstdio>

#define DEPCHECK_STR 520

enum EventKind {
    EV_ATTACH = 0,   // this process started (a = exe path)
    EV_EXEC,         // CreateProcess call    (a = app, b = cmdline, aux = child pid)
    EV_OPEN,         // CreateFileW call      (a = path, n1 = disposition, aux = ok)
    EV_LOAD,         // LoadLibraryExW call   (a = name, n1 = flags, aux = ok)
    EV_META          // final event           (aux = dropped-event count)
};

struct TraceEvent {
    ULONGLONG ts;
    DWORD pid;
    DWORD aux;
    DWORD n1;
    EventKind kind;
    wchar_t a[DEPCHECK_STR];
    wchar_t b[2048];
};

static const size_t RING_CAP = 1024;
static TraceEvent g_ring[RING_CAP];
static size_t g_head = 0, g_count = 0;
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
static volatile LONG g_stop = 0;
static volatile LONG g_dropped = 0;

static wchar_t g_dll_path[DEPCHECK_STR];
static wchar_t g_reports_dir[DEPCHECK_STR];

// ---------------- event queue ----------------

static void push_event(TraceEvent& ev)
{
    BOOL queued = FALSE;
    AcquireSRWLockExclusive(&g_lock);
    if (g_count < RING_CAP) {
        g_ring[(g_head + g_count) % RING_CAP] = ev;
        g_count++;
        queued = TRUE;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (queued)
        WakeConditionVariable(&g_cv);
    else
        InterlockedIncrement(&g_dropped);
}

// ---------------- trace writer ----------------

// Escape s (UTF-8) into out as a JSON string body (no quotes).
static void json_escape(const char* s, char* out, size_t cap)
{
    size_t o = 0;
    for (const char* p = s; *p && o + 7 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)  { o += (size_t)sprintf(out + o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void write_line(HANDLE f, const char* s)
{
    DWORD written = 0;
    WriteFile(f, s, (DWORD)strlen(s), &written, NULL);
}

static void format_and_write(HANDLE f, const TraceEvent& ev)
{
    char ua[DEPCHECK_STR * 3], ub[2048 * 3], line[8192];

    WideCharToMultiByte(CP_UTF8, 0, ev.a, -1, ua, sizeof(ua), NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, ev.b, -1, ub, sizeof(ub), NULL, NULL);

    char ea[DEPCHECK_STR * 6], eb[2048 * 6];
    json_escape(ua, ea, sizeof(ea));
    json_escape(ub, eb, sizeof(eb));

    switch (ev.kind) {
    case EV_ATTACH:
        sprintf_s(line, "{\"ts\":%llu,\"pid\":%lu,\"ev\":\"attach\",\"exe\":\"%s\"}\n",
                  (unsigned long long)ev.ts, (unsigned long)ev.pid, ea);
        break;
    case EV_EXEC:
        sprintf_s(line, "{\"ts\":%llu,\"pid\":%lu,\"ev\":\"exec\",\"app\":\"%s\","
                  "\"cmd\":\"%s\",\"child\":%lu,\"inj\":%lu}\n",
                  (unsigned long long)ev.ts, (unsigned long)ev.pid, ea, eb,
                  (unsigned long)ev.aux, (unsigned long)ev.n1);
        break;
    case EV_OPEN:
        sprintf_s(line, "{\"ts\":%llu,\"pid\":%lu,\"ev\":\"open\",\"path\":\"%s\","
                  "\"disp\":%lu,\"ok\":%lu}\n",
                  (unsigned long long)ev.ts, (unsigned long)ev.pid, ea,
                  (unsigned long)ev.n1, (unsigned long)ev.aux);
        break;
    case EV_LOAD:
        sprintf_s(line, "{\"ts\":%llu,\"pid\":%lu,\"ev\":\"load\",\"name\":\"%s\","
                  "\"flags\":%lu,\"ok\":%lu}\n",
                  (unsigned long long)ev.ts, (unsigned long)ev.pid, ea,
                  (unsigned long)ev.n1, (unsigned long)ev.aux);
        break;
    case EV_META:
        sprintf_s(line, "{\"ts\":%llu,\"pid\":%lu,\"ev\":\"meta\",\"dropped\":%lu}\n",
                  (unsigned long long)ev.ts, (unsigned long)ev.pid,
                  (unsigned long)ev.aux);
        break;
    }
    write_line(f, line);
}

// FILE_SHARE_DELETE so a later run can clean up stale traces even if a
// long-lived child (mspdbsrv.exe) still holds this file open.
static DWORD WINAPI drain_thread(LPVOID)
{
    wchar_t path[DEPCHECK_STR + 64];
    swprintf_s(path, _countof(path), L"%ls\\trace-%lu.jsonl",
               g_reports_dir, GetCurrentProcessId());
    HANDLE f = CreateFileW(path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_DELETE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;

    for (;;) {
        TraceEvent ev;
        BOOL have = FALSE;

        AcquireSRWLockExclusive(&g_lock);
        while (g_count == 0 && !g_stop)
            SleepConditionVariableSRW(&g_cv, &g_lock, 50, 0);
        if (g_count > 0) {
            ev = g_ring[g_head];
            g_head = (g_head + 1) % RING_CAP;
            g_count--;
            have = TRUE;
        }
        ReleaseSRWLockExclusive(&g_lock);

        if (have)
            format_and_write(f, ev);
        else if (g_stop)
            break;
    }

    FlushFileBuffers(f);
    CloseHandle(f);
    return 0;
}

// ---------------- exec-target architecture guard (doc: architecture matrix) ----

// True when the child should be launched with our DLL grafted in. Anything we
// cannot positively identify as a non-AMD64 PE gets the DLL (optimistic); a
// confirmed x86/ARM64 child runs unhooked rather than failing to start.
static bool ShouldInject(LPCWSTR app, LPCWSTR cmdline)
{
    wchar_t target[DEPCHECK_STR] = L"";

    if (app && *app) {
        wcsncpy_s(target, app, _TRUNCATE);
    } else if (cmdline && *cmdline) {
        // first token of the command line, honoring quotes
        const wchar_t* p = cmdline;
        size_t o = 0;
        bool quoted = false;
        while (*p && o + 1 < DEPCHECK_STR) {
            if (*p == L'"') { quoted = !quoted; p++; continue; }
            if (!quoted && (*p == L' ' || *p == L'\t')) break;
            target[o++] = *p++;
        }
        target[o] = 0;
    } else {
        return true;
    }
    if (!target[0])
        return true;

    wchar_t resolved[DEPCHECK_STR];
    if (wcschr(target, L'\\') || wcschr(target, L'/')) {
        wcsncpy_s(resolved, target, _TRUNCATE);
    } else {
        // bare name: resolve through the search path like CreateProcess would
        if (!SearchPathW(NULL, target, L".exe", DEPCHECK_STR, resolved, NULL))
            return true;   // not on PATH — App Paths / PATHEXT may still find it
    }

    HANDLE f = CreateFileW(resolved, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return true;

    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    DWORD got = 0;
    BOOL ok = ReadFile(f, &dos, sizeof(dos), &got, NULL) && got == sizeof(dos)
           && dos.e_magic == IMAGE_DOS_SIGNATURE
           && SetFilePointer(f, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER
           && ReadFile(f, &nt, sizeof(nt), &got, NULL) && got == sizeof(nt)
           && nt.Signature == IMAGE_NT_SIGNATURE;
    CloseHandle(f);

    if (!ok)
        return true;                       // not a PE (script?) — let CreateProcess wrap it

    // Batch scripts: the kernel wraps them in cmd.exe implicitly and that
    // wrapping corrupts cmd's parsing under the graft — never inject; the
    // wrapping cmd.exe runs unhooked.
    const wchar_t* ext = wcsrchr(resolved, L'.');
    if (ext && (_wcsicmp(ext, L".bat") == 0 || _wcsicmp(ext, L".cmd") == 0))
        return false;

    return nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
}

// ---------------- the hooks ----------------

static BOOL (WINAPI *Real_CreateProcessW)(
    LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION)
    = CreateProcessW;

static BOOL (WINAPI *Real_CreateProcessA)(
    LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES,
    BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION)
    = CreateProcessA;

static HANDLE (WINAPI *Real_CreateFileW)(
    LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE)
    = CreateFileW;

static HMODULE (WINAPI *Real_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD)
    = LoadLibraryExW;

static void LogExecW(LPCWSTR app, LPCWSTR cmd, const PROCESS_INFORMATION* pi,
                     BOOL ok, DWORD injected)
{
    TraceEvent ev = {};
    ev.ts = GetTickCount64();
    ev.pid = GetCurrentProcessId();
    ev.aux = (ok && pi) ? pi->dwProcessId : 0;
    ev.n1 = injected;
    ev.kind = EV_EXEC;
    wcsncpy_s(ev.a, app ? app : L"", _TRUNCATE);
    wcsncpy_s(ev.b, cmd ? cmd : (app ? app : L""), _TRUNCATE);
    push_event(ev);
}

static void LogExecA(LPCSTR app, LPCSTR cmd, const PROCESS_INFORMATION* pi,
                     BOOL ok, DWORD injected)
{
    wchar_t wapp[DEPCHECK_STR] = L"", wcmd[2048] = L"";
    if (app) MultiByteToWideChar(CP_ACP, 0, app, -1, wapp, DEPCHECK_STR);
    if (cmd) MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd, 2048);
    LogExecW(app ? wapp : NULL, cmd ? wcmd : NULL, pi, ok, injected);
}

BOOL WINAPI Hook_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES psa,
    LPSECURITY_ATTRIBUTES pta, BOOL inherit, DWORD flags, LPVOID env, LPCWSTR cwd,
    LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    BOOL ok;
    DWORD injected = 0;
    if (!(flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS))
        && ShouldInject(app, cmd)) {
        // DetourCreateProcessWithDllExW takes the DLL name as LPCSTR even in
        // the wide form (a Detours quirk).
        char adll[DEPCHECK_STR * 3];
        WideCharToMultiByte(CP_ACP, 0, g_dll_path, -1, adll, sizeof(adll),
                            NULL, NULL);
        // The doc's propagation chain: re-launch THROUGH Detours so the child
        // carries this DLL. Real_CreateProcessW (the trampoline) is the spawn
        // routine — passing NULL would recurse into this hook forever.
        ok = DetourCreateProcessWithDllExW(app, cmd, psa, pta, inherit, flags,
                                           env, cwd, si, pi, adll,
                                           Real_CreateProcessW);
        if (ok) {
            injected = 1;
        } else {
            // never break the child because of us — fall back to a plain spawn
            ok = Real_CreateProcessW(app, cmd, psa, pta, inherit, flags, env,
                                     cwd, si, pi);
        }
    } else {
        ok = Real_CreateProcessW(app, cmd, psa, pta, inherit, flags, env,
                                 cwd, si, pi);
    }
    LogExecW(app, cmd, pi, ok, injected);
    return ok;
}

BOOL WINAPI Hook_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES psa,
    LPSECURITY_ATTRIBUTES pta, BOOL inherit, DWORD flags, LPVOID env, LPCSTR cwd,
    LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    BOOL ok;
    DWORD injected = 0;
    wchar_t wapp[DEPCHECK_STR] = L"";
    wchar_t wcmd[2048] = L"";
    if (app) MultiByteToWideChar(CP_ACP, 0, app, -1, wapp, DEPCHECK_STR);
    if (cmd) MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd, 2048);

    if (!(flags & (DEBUG_PROCESS | DEBUG_ONLY_THIS_PROCESS))
        && ShouldInject(app ? wapp : NULL, cmd ? wcmd : NULL)) {
        char adll[DEPCHECK_STR * 3];
        WideCharToMultiByte(CP_ACP, 0, g_dll_path, -1, adll, sizeof(adll),
                            NULL, NULL);
        ok = DetourCreateProcessWithDllExA(app, cmd, psa, pta, inherit, flags,
                                           env, cwd, si, pi, adll,
                                           Real_CreateProcessA);
        if (ok) {
            injected = 1;
        } else {
            ok = Real_CreateProcessA(app, cmd, psa, pta, inherit, flags, env,
                                     cwd, si, pi);
        }
    } else {
        ok = Real_CreateProcessA(app, cmd, psa, pta, inherit, flags, env,
                                 cwd, si, pi);
    }
    LogExecA(app, cmd, pi, ok, injected);
    return ok;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags_, HANDLE tmpl)
{
    HANDLE h = Real_CreateFileW(path, access, share, sa, disp, flags_, tmpl);
    TraceEvent ev = {};
    ev.ts = GetTickCount64();
    ev.pid = GetCurrentProcessId();
    ev.aux = (h != INVALID_HANDLE_VALUE) ? 1 : 0;
    ev.n1 = disp;
    ev.kind = EV_OPEN;
    wcsncpy_s(ev.a, path ? path : L"", _TRUNCATE);
    push_event(ev);
    return h;
}

HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE m = Real_LoadLibraryExW(name, file, flags);
    TraceEvent ev = {};
    ev.ts = GetTickCount64();
    ev.pid = GetCurrentProcessId();
    ev.aux = (m != NULL) ? 1 : 0;
    ev.n1 = flags;
    ev.kind = EV_LOAD;
    wcsncpy_s(ev.a, name ? name : L"", _TRUNCATE);
    push_event(ev);
    return m;
}

// ---------------- installation ----------------

// Detours grafts an import-by-ordinal-1 for this DLL into every child it
// launches (DetourCreateProcessWithDll*); the child's loader rejects the
// image unless the payload exports ordinal 1. See depcheck.def.
extern "C" __declspec(dllexport) void WINAPI DepCheckMarker(void)
{
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        DetourRestoreAfterWith();

        GetModuleFileNameW(h, g_dll_path, DEPCHECK_STR);
        DWORD n = GetEnvironmentVariableW(L"DEPCHECK_REPORTS", g_reports_dir,
                                          DEPCHECK_STR);
        if (n == 0 || n >= DEPCHECK_STR)
            wcscpy_s(g_reports_dir, L"reports");

        // drain thread starts once DllMain returns (loader lock); every event
        // queued before that simply waits in the ring
        HANDLE t = CreateThread(NULL, 0, drain_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);

        {
            TraceEvent ev = {};
            ev.ts = GetTickCount64();
            ev.pid = GetCurrentProcessId();
            ev.kind = EV_ATTACH;
            GetModuleFileNameW(NULL, ev.a, DEPCHECK_STR);
            push_event(ev);
        }

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)Real_CreateProcessW, Hook_CreateProcessW);
        DetourAttach(&(PVOID&)Real_CreateProcessA, Hook_CreateProcessA);
        DetourAttach(&(PVOID&)Real_CreateFileW,   Hook_CreateFileW);
        DetourAttach(&(PVOID&)Real_LoadLibraryExW, Hook_LoadLibraryExW);
        DetourTransactionCommit();
        return TRUE;
    }
    if (reason == DLL_PROCESS_DETACH) {
        TraceEvent ev = {};
        ev.ts = GetTickCount64();
        ev.pid = GetCurrentProcessId();
        ev.kind = EV_META;
        ev.aux = (DWORD)g_dropped;
        push_event(ev);
        InterlockedExchange(&g_stop, 1);
        WakeConditionVariable(&g_cv);
        // bounded flush window: at process exit the drain thread may already
        // be gone (other threads are terminated before DLL detach)
        Sleep(100);
    }
    return TRUE;
}
