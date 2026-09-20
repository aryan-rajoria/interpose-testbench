#!/bin/sh
# Build the 2 universal interposer artifacts into dist/ using docker.
# One .so per CPU architecture, valid across glibc (any version) and musl:
#
#   libinterpose-x86_64.so    ubuntu 20.04/22.04/24.04, alpine
#   libinterpose-aarch64.so   ubuntu 24.04 arm64, alpine arm64
#
# See interpose/build.sh for why they must be built on musl.
set -eu

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"
mkdir -p dist

# Images must already exist (docker/Dockerfile); CI builds them first.
ALPINE_X86=interpose:alpine-musl
ALPINE_ARM=interpose:alpine-musl-arm64

echo "== universal artifacts (zero DT_NEEDED, weak dlsym + _r_debug fallback) =="
docker run --rm --platform linux/amd64 -v "$REPO_ROOT:/repo" -w /repo "$ALPINE_X86" \
    sh interpose/build.sh dist/libinterpose-x86_64.so
docker run --rm --platform linux/arm64 -v "$REPO_ROOT:/repo" -w /repo "$ALPINE_ARM" \
    sh interpose/build.sh dist/libinterpose-aarch64.so

ls -la dist/
