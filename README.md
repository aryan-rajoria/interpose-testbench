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

## Prebuilt artifacts (3 `.so` files)

The bench does **not** rebuild the shim per container. One `.so` cannot cover
glibc *and* musl in general: a glibc-built `.so` carries `DT_NEEDED libc.so.6`
plus versioned symbol references (`execvp@GLIBC_2.x`) that musl cannot
satisfy. Bear (our reference) solves this the same way — it builds its
`libexec.so` **per target libc**, and on glibc < 2.34 relies on its link line
adding `DT_NEEDED libdl.so.2`, which drags `libdl` (and `dlsym`) into every
process the library is preloaded into.

We exploit the reverse asymmetry: a `.so` built on musl with **zero
`DT_NEEDED`** and only unversioned, glibc/musl-overlapping symbols binds its
`dlsym`/`exec*` references to whichever libc the host process already uses.
That single artifact works on musl and on glibc >= 2.34 (where `dlsym` moved
into `libc.so.6`). Only glibc < 2.34 (Ubuntu 20.04) needs the Bear-style
`-ldl` artifact. Result — 3 artifacts, built once per PR:

| artifact | built in | used by | why |
|---|---|---|---|
| `libinterpose-musl-x86_64.so` | alpine | ubuntu 22.04/24.04, alpine | zero DT_NEEDED, binds to host libc |
| `libinterpose-musl-aarch64.so` | alpine/arm64 | ubuntu 24.04-arm64, alpine-arm64 | same |
| `libinterpose-glibc-x86_64.so` | ubuntu 20.04 | ubuntu 20.04 | glibc 2.31: `dlsym` lives in `libdl.so.2`, so the artifact must pull it in |

Rebuild locally with `sh tests/build_artifacts.sh` (requires the scenario
images). The CI `build-artifacts` job verifies the musl artifacts have no
`DT_NEEDED` and the glibc one depends on `libdl.so.2`. Scenario jobs consume
the mapped artifact via `INTERPOSE_LIB`.

Known limitation: the musl artifacts don't export `execvpe` (musl has none),
so `execvpe` callers on glibc >= 2.34 run unintercepted; the covered build
systems don't use it.

## Run locally

```sh
sh tests/build_artifacts.sh   # builds dist/*.so in the scenario images
docker build --build-arg BASE_IMAGE=ubuntu:24.04 -f docker/Dockerfile -t interpose:ubuntu-24.04-glibc .
docker run --rm -v "$PWD:/src" -w /src \
    -e INTERPOSE_LIB=/src/dist/libinterpose-musl-x86_64.so \
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
# smoke
