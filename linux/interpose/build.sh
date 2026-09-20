#!/bin/sh
# Build the universal interposer .so: one artifact per CPU architecture,
# usable on glibc (any version) and musl. MUST run on musl (e.g. an
# alpine container) so the artifact has no libc DT_NEEDED entry and no
# versioned symbol references; its undefined symbols then bind to
# whichever libc the preloaded process already uses.
#
#   docker run --rm --platform linux/amd64 -v "$PWD:/repo" -w /repo \
#       interpose:alpine-musl sh linux/interpose/build.sh linux/dist/libinterpose-x86_64.so
#
# Out: <name> next to this script's cwd.
set -eu

out=${1:-libinterpose.so}
cc -shared -fPIC -O2 -fno-plt -fno-stack-protector -nostdlib \
   -Wl,--as-needed \
   -o "$out" "$(dirname "$0")/interpose.c"

fail() { echo "FAIL: $*" >&2; exit 1; }

# sanity 1: the artifact must not depend on any shared library
if readelf -d "$out" 2>/dev/null | grep -q NEEDED; then
    fail "$out has DT_NEEDED entries (not libc-portable)"
fi

# sanity 2: dlsym/_r_debug must be weak refs (else glibc < 2.34 without
# libdl would abort on symbol lookup instead of taking the fallback path)
for sym in dlsym _r_debug; do
    line=$(readelf -Ws "$out" | awk -v s="$sym" '$7=="UND" && $8==s { print $5 }' | sort -u)
    [ "$line" = "WEAK" ] || fail "undefined $sym is not a weak reference"
done

# sanity 3: dlsym address checks need a GOT slot, not a PLT stub.
# (readelf truncates relocation names on some arches: GLOB_DA(T).)
readelf -r "$out" | grep -w dlsym | grep -q 'GLOB_DA' || \
    fail "dlsym does not use a GOT slot (compile with -fno-plt)"

echo "built $out ($(readelf -h "$out" | awk '/Machine:/ { $1 = ""; print }'))"
