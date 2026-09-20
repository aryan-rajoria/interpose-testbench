# windows/ — Detours-based process-creation interception

A hands-on companion to the `windows-monitoring` doc
`04-detours-api-hooking.md`: the doc's Detours interposer, actually built
and run against a real project. This is the Windows counterpart of
`linux/` (env-only `LD_PRELOAD` there, DLL injection here).

## Layout

```
fwflash/       target project (low-level Win32 C, no CRT file I/O)
  src/         fwflash.exe — FWFL firmware image create/flash
  device/      device.dll — simulated NOR-flash device plugin (LoadLibrary'd at runtime)
  worker/      worker.exe — child process that re-verifies the flash
depcheck/      the interposer
  src/payload.cpp   depcheck.dll — hooks + event ring + drain thread
  src/launcher.cpp  depcheck.exe — injects the DLL, waits, merges the report
vendor/detours/    Microsoft Detours subset (headers + built detours.lib — see its README)
tests/run_scenario.cmd   short CI-style scenario (build, trace, assert)
docker/Dockerfile       windows scenario image (Server Core + VS Build Tools)
msvc-env.cmd   enters the VS dev environment (vswhere; VS-18 fallback)
run-demo.cmd   build everything, trace the build, trace a flash run
reports/       <dir>\trace-<pid>.jsonl per traced process + report.md / report.json (generated)
```

## What the interposer does

- Hooks `CreateProcessW` **and** `CreateProcessA` (exec funnel), `CreateFileW`
  (file opens), `LoadLibraryExW` (runtime DLL loads).
- Every spawn is re-launched through `DetourCreateProcessWithDllExW` with the
  payload's own path, so the DLL propagates through the **entire** process
  tree — the Windows equivalent of `LD_PRELOAD`'s env inheritance.
- Hooks never do file I/O (loader lock): events land in a ring buffer and a
  drain thread (started after attach) writes `trace-<pid>.jsonl`.
- The child's PE machine type is checked before injection (the doc's
  architecture matrix); non-AMD64 or `.bat`/`.cmd` targets run unhooked
  instead of failing.
- `depcheck.exe [reports-dir] -- <command line>` launches the root, waits,
  merges all traces into `report.md` / `report.json`: the **chronological
  list of every exec call**, the process tree, runtime DLL loads, file-open
  histogram, and a `dumpbin /dependents` static-vs-runtime cross-check.

## Run it

On a machine with VS + the C++ toolchain (from `windows/`):

```
cmd /c run-demo.cmd
```

- `reports\build\report.md` — everything spawned while building fwflash
  (vcvars → vswhere/reg/cmd/powershell → cl → link → cvtres → mspdbsrv …)
- `reports\flash\report.md` — fwflash.exe runtime: device.dll loaded via
  `LoadLibraryExW` (invisible to static imports), the delay-loaded
  bcrypt.dll, image/device file opens, and the worker.exe child.

## Test scenario (CI-style, also used by the container test)

```
cmd /c tests\run_scenario.cmd
```

Builds depcheck, traces a fwflash build and a create+flash run, then
asserts on the merged reports (cl.exe/vswhere.exe execs in the build trace;
worker.exe child and device.dll runtime load in the flash trace). Exits
nonzero on any failure — mirrors `linux/tests/run_scenario.sh`.

## Windows container

`docker/Dockerfile` builds a `windowsservercore` + VS Build Tools image
(needs a Windows container host; the image tag must match the host OS
build):

```
docker build -f windows/docker/Dockerfile -t interpose:windows-2022-x64 .
docker run --rm -v C:\path\to\repo:C:\src -w C:\src interpose:windows-2022-x64 ^
    cmd /c windows\tests\run_scenario.cmd
```

`.github/workflows/pr-windows.yml` wires this up as a CI job, currently
`workflow_dispatch`-only because no Windows runner is registered for the
repo.

## Gotchas hit while building this (all fixed, kept here as documentation)

1. **Payload must export ordinal 1.** `DetourCreateProcessWithDll*` grafts an
   import-by-ordinal-1 into the child; without that export the child dies
   with `0xC000007B` (STATUS_INVALID_IMAGE_FORMAT). The doc's skeleton never
   mentions this — see `depcheck/src/depcheck.def`.
2. **Pass the trampoline as the spawn routine.** Inside a `CreateProcessW`
   hook you must call `DetourCreateProcessWithDllExW(..., Real_CreateProcessW)`;
   passing NULL makes Detours call the hooked `CreateProcessW` again →
   infinite recursion. This is what Detours' own `trcbld` sample does.
3. **`DetourCreateProcessWithDll*W` takes the DLL name as LPCSTR** even in
   the wide variants.
4. **Don't `SetConsoleOutputCP(CP_UTF8)` in the launcher.** The traced
   cmd.exe inherits the console; under a UTF-8 codepage cmd misreads batch
   scripts with a constant byte offset (lines execute 6+ bytes late).
5. **Batch targets need an explicit `cmd.exe /s /c "..."` wrap** — handing a
   `.cmd` straight to `CreateProcessW` plus the graft corrupts cmd's parsing.
6. **Detours vcpkg-free build**: `nmake` in a vcvars64 shell; the samples
   stage needs `sn.exe` (skip it — only the core lib is required, see
   `vendor/detours/README.md`).

## Known blind spots (per the doc)

- Kernel32-layer hooks: direct `ntdll!NtCreateUserProcess`/`NtCreateFile`
  callers bypass them (rare; static binaries likewise).
- Static imports of a process are not seen as `LoadLibraryExW` calls — that's
  what the dumpbin cross-check section is for.
