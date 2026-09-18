#!/bin/sh
# Build the portable interposer .so. MUST run on musl (e.g. an alpine
# container) so the artifact has no libc DT_NEEDED entry and no versioned
# symbol references; its undefined symbols then bind to whichever libc the
# preloaded process already uses (glibc or musl).
#
#   docker run --rm -v "$PWD/interpose:/src" -w /src interpose:alpine-musl \
#       sh build.sh libinterpose-x86_64.so
#
# Out: <name>.so next to this script.
set -eu

out=${1:-libinterpose.so}
cc -shared -fPIC -O2 -fno-stack-protector -nostdlib \
   -Wl,--as-needed \
   -o "$out" "$(dirname "$0")/interpose.c"

# sanity: the artifact must not depend on any shared library
if readelf -d "$out" 2>/dev/null | grep -q NEEDED; then
    echo "FAIL: $out has DT_NEEDED entries (not libc-portable):" >&2
    readelf -d "$out" | grep NEEDED >&2
    exit 1
fi
echo "built $out ($(readelf -h "$out" | sed -n 's/^Machine:.*:[[:space:]]*//p'))"
