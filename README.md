# interpose-testbench

A minimal test bench for `LD_PRELOAD`-based process-creation interception,
exercised across distros, libcs (glibc/musl), architectures (x86_64/aarch64
under QEMU), and build systems.

## How it works

- `interpose/interpose.c` — a tiny home-grown shim (Bear used as reference
  only) that intercepts the `exec*`, `posix_spawn*`, `popen`, and `system`
  families and appends one line per call to `$INTERPOSE_LOG`.
- Interception is **env-only**: the harness exports `LD_PRELOAD` and
  `INTERPOSE_LOG` and runs the build command **directly**. No wrapper or
  dispatcher sits in front of `make`/`cmake`/etc. Env inheritance keeps every
  child process (compilers, shell snippets, linker) intercepted.
- `tests/run_scenario.sh` runs each tiny build-system project and asserts the
  expected compiler invocations appear in the log. By default it consumes a
  prebuilt artifact via `INTERPOSE_LIB`; unset, it compiles the shim
  in-container with the native toolchain instead.

## Scenarios

| scenario | base image | arch |
|---|---|---|
| ubuntu-20.04-glibc | ubuntu:20.04 | x86_64 |
| ubuntu-22.04-glibc | ubuntu:22.04 | x86_64 |
| ubuntu-24.04-glibc | ubuntu:24.04 | x86_64 |
| alpine-musl | alpine:latest | x86_64 |
| ubuntu-24.04-glibc-arm64 | ubuntu:24.04 | aarch64 (qemu-user-static) |
| alpine-musl-arm64 | alpine:latest | aarch64 (qemu-user-static) |

Build systems: `make`, `cmake` (Makefile + Ninja generators), `autotools`,
`meson`, bare `ninja`.

## Universal artifacts (1 `.so` per CPU architecture)

Exactly two artifacts cover all 36 scenario combinations:

| artifact | built in | covers |
|---|---|---|
| `libinterpose-x86_64.so` | alpine (musl) | ubuntu 20.04/22.04/24.04, alpine |
| `libinterpose-aarch64.so` | alpine/arm64 (musl) | ubuntu 24.04 arm64, alpine arm64 |

Each is built on musl with `-nostdlib`, so it has **no `DT_NEEDED`** and no
versioned symbol references; its undefined symbols (`exec*`, `snprintf`, ...)
bind to whichever libc the host process already uses — glibc or musl.

Why a resolver fallback: the real-function lookup normally uses `dlsym`,
but glibc < 2.34 keeps `dlsym` in `libdl.so.2`, and processes like Ubuntu
20.04's `cc` and `/bin/sh` don't link it — a plain `dlsym` reference aborts
those processes with `symbol lookup error` (this is why Bear builds its
`libexec.so` per target libc and leans on the Rust link line to pull in
`libdl.so.2` on old glibc). Instead:

- `dlsym` is declared **weak** and the .so is compiled with `-fno-plt`, so
  the code tests whether it actually resolved before calling it;
- when it didn't (glibc < 2.34 without libdl), a small built-in resolver
  walks the loader's `_r_debug` link_map, identifies its own object via
  `_DYNAMIC`, and scans each later object's `.dynsym`/`.dynstr` via
  `DT_HASH`/`DT_GNU_HASH` — a `dlsym(RTLD_NEXT)` equivalent with no libdl
  dependency at all.

`interpose/build.sh` enforces this at build time: zero `DT_NEEDED`, weak
`dlsym`/`_r_debug` references, and a GOT slot (not a PLT stub) for `dlsym`.

Known limitation: the artifacts don't export `execvpe` (musl has none), so
`execvpe` callers on glibc run unintercepted; the covered build systems
don't use it.

## Run locally

```sh
sh tests/build_artifacts.sh   # builds dist/*.so in alpine containers
docker build --build-arg BASE_IMAGE=ubuntu:24.04 -f docker/Dockerfile -t interpose:ubuntu-24.04-glibc .
docker run --rm -v "$PWD:/src" -w /src \
    -e INTERPOSE_LIB=/src/dist/libinterpose-x86_64.so \
    interpose:ubuntu-24.04-glibc sh tests/run_scenario.sh
```

arm64 scenarios need binfmt handlers registered once on the host (no host
qemu install required):

```sh
docker run --privileged --rm tonistiigi/binfmt --install arm64
```

## CI

`.github/workflows/pr.yml` runs the full `scenario × build_system` matrix on a
self-hosted runner for every pull request, and is meant to be a required
status check before merge.

## Timing

Every run finishes with a **Timing report** job that appends a step summary:
total wall clock, sum of all job durations, and per-job seconds. Since one
self-hosted runner executes jobs serially, wall clock ≈ sum of jobs; the two
numbers diverge only if you register more runner replicas.

From the CLI:

```sh
gh run view <run-id> --repo aryan-rajoria/interpose-testbench \
    --json jobs --jq '.jobs[] | [.name, .startedAt, .completedAt]'
```
