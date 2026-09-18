#!/bin/sh
# Build the 3 prebuilt interposer artifacts into dist/ using docker.
#
#   libinterpose-musl-x86_64.so    universal: musl + glibc >= 2.34
#   libinterpose-musl-aarch64.so   universal: musl + glibc >= 2.34
#   libinterpose-glibc-x86_64.so   Bear-style -ldl build for glibc < 2.34
#                                  (built on the oldest glibc of the matrix)
#
# Scenario -> artifact mapping (also used by CI):
#   ubuntu-20.04-glibc        -> glibc-x86_64   (glibc 2.31: dlsym needs libdl)
#   ubuntu-22.04-glibc        -> musl-x86_64
#   ubuntu-24.04-glibc        -> musl-x86_64
#   alpine-musl               -> musl-x86_64
#   ubuntu-24.04-glibc-arm64  -> musl-aarch64
#   alpine-musl-arm64         -> musl-aarch64
set -eu

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO_ROOT"
mkdir -p dist

# Image must already exist (docker/Dockerfile); CI builds them first.
ALPINE_X86=interpose:alpine-musl
ALPINE_ARM=interpose:alpine-musl-arm64
GLIBC_X86=interpose:ubuntu-20.04-glibc

echo "== musl universal artifacts (zero DT_NEEDED) =="
docker run --rm --platform linux/amd64  -v "$REPO_ROOT:/repo" -w /repo "$ALPINE_X86" \
    sh interpose/build.sh dist/libinterpose-musl-x86_64.so
docker run --rm --platform linux/arm64 -v "$REPO_ROOT:/repo" -w /repo "$ALPINE_ARM" \
    sh interpose/build.sh dist/libinterpose-musl-aarch64.so

echo "== glibc <2.34 artifact (DT_NEEDED libdl.so.2, Bear-style) =="
docker run --rm --platform linux/amd64 -v "$REPO_ROOT:/repo" -w /repo "$GLIBC_X86" \
    sh -c 'cc -shared -fPIC -O2 -o dist/libinterpose-glibc-x86_64.so interpose/interpose.c -ldl \
        && readelf -d dist/libinterpose-glibc-x86_64.so | grep NEEDED'

ls -la dist/
