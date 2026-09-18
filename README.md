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
- `tests/run_scenario.sh` compiles the shim with the *native* toolchain of the
  container it runs in, so the ABI (glibc vs musl, x86_64 vs aarch64) always
  matches, then runs each tiny build-system project and asserts the expected
  compiler invocations appear in the log.

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

## Run locally

```sh
docker build --build-arg BASE_IMAGE=ubuntu:24.04 -f docker/Dockerfile -t interpose:u24 .
docker run --rm -v "$PWD:/src" -w /src interpose:u24 sh tests/run_scenario.sh
```

arm64 scenarios need binfmt handlers registered once on the host:

```sh
sudo apt-get install -y qemu-user-static
docker run --privileged --rm tonistiigi/binfmt --install arm64
```

## CI

`.github/workflows/pr.yml` runs the full `scenario × build_system` matrix on a
self-hosted runner for every pull request, and is meant to be a required
status check before merge.
