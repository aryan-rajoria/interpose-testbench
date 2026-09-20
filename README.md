# interpose-testbench

A testbench for process-creation interception, with one strictly separated
half per desktop-server platform. Both halves answer the same question —
"can we see every process a build/runtime spawns, and assert on it?" — with
each OS's native mechanism.

- **linux/** — env-only `LD_PRELOAD` interception (`linux/interpose/`),
  exercised across distros, libcs (glibc/musl), architectures
  (x86_64/aarch64 under QEMU), and build systems. → `linux/README.md`
- **windows/** — Detours-based DLL injection (`windows/depcheck/`) that
  propagates through the whole process tree, tracing a small Win32
  project's build and runtime (`windows/fwflash/`). → `windows/README.md`

## Layout

```
linux/
  interpose/    the LD_PRELOAD shim + universal-artifact build
  tests/        per-build-system scenario runner + artifact builder
  docker/       parametrized scenario images (BASE_IMAGE build-arg)
  dist/         prebuilt universal artifacts (1 .so per arch, generated)
windows/
  depcheck/     the Detours interposer (payload dll + launcher exe)
  fwflash/      target project traced by the scenarios
  vendor/       Microsoft Detours subset (headers + detours.lib)
  tests/        CI-style scenario: build, trace, assert
  docker/       windows scenario image (Server Core + VS Build Tools)
```

## CI

- `.github/workflows/pr-linux.yml` — full `scenario × build_system` matrix
  (6 distro/arch scenarios × 6 build systems) on a self-hosted Linux
  runner; required status check for PRs.
- `.github/workflows/pr-windows.yml` — the `windows-2022-x64` Detours
  scenario in a Windows container. Currently `workflow_dispatch`-only
  because no Windows runner is registered for the repo; enable it once one
  exists.
