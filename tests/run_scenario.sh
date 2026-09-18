#!/bin/sh
# run_scenario.sh [build_system ...]
#
# Builds libinterpose.so with the native toolchain of whatever distro this
# script runs in, then executes each build system's build command DIRECTLY
# with only LD_PRELOAD + INTERPOSE_LOG set in the environment (no wrapper,
# no dispatcher). Asserts that compiler invocations show up in the log.
set -u

REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
LOG="$WORK/interpose.log"
FAIL=0

echo "== environment =="
cat /etc/os-release 2>/dev/null | head -2 || true
uname -m
cc --version | head -1

echo "== building libinterpose.so =="
cc -shared -fPIC -O2 -o "$WORK/libinterpose.so" "$REPO_ROOT/interpose/interpose.c" -ldl || exit 2

stage() { # stage <name> : copy common sources + project files
    name=$1
    dir="$WORK/$name"
    mkdir -p "$dir"
    cp "$REPO_ROOT"/tests/projects/common/*.c "$dir/"
    cp "$REPO_ROOT"/tests/projects/"$name"/* "$dir/" 2>/dev/null || true
    echo "$dir"
}

assert_log() { # assert_log <label> <grep-pattern> [min-count]
    label=$1; pattern=$2; min=${3:-1}
    count=$(grep -c -- "$pattern" "$LOG" 2>/dev/null || true)
    count=${count:-0}
    if [ "$count" -lt "$min" ]; then
        echo "FAIL [$label]: expected >= $min log line(s) matching '$pattern', got $count"
        echo "--- last 20 log lines ---"
        tail -20 "$LOG" 2>/dev/null
        FAIL=1
    else
        echo "PASS [$label]: $count line(s) matched '$pattern'"
    fi
}

run_build() { # run_build <dir> <cmd...>  — env-only interception
    (cd "$1" && shift && LD_PRELOAD="$WORK/libinterpose.so" INTERPOSE_LOG="$LOG" "$@")
}

reset_log() { rm -f "$LOG"; : > "$LOG"; }

scenario_make() {
    d=$(stage make)
    reset_log
    run_build "$d" make -s >/dev/null ||
        { echo "FAIL [make]: build command failed"; FAIL=1; return; }
    assert_log make "greet2.c" 1
    assert_log make "hello.c" 1
    assert_log make "hello.o greet2.o" 1
}

scenario_cmake_make() {
    d=$(stage cmake)
    reset_log
    run_build "$d" cmake -S . -B bmake -G "Unix Makefiles" >/dev/null ||
        { echo "FAIL [cmake-make]: configure failed"; FAIL=1; return; }
    run_build "$d" cmake --build bmake >/dev/null ||
        { echo "FAIL [cmake-make]: build failed"; FAIL=1; return; }
    assert_log cmake-make "greet2.c" 1
    assert_log cmake-make "hello.c" 1
}

scenario_cmake_ninja() {
    d=$(stage cmake)
    rm -rf "$d/bninja"
    reset_log
    run_build "$d" cmake -S . -B bninja -G Ninja >/dev/null ||
        { echo "FAIL [cmake-ninja]: configure failed"; FAIL=1; return; }
    run_build "$d" cmake --build bninja >/dev/null ||
        { echo "FAIL [cmake-ninja]: build failed"; FAIL=1; return; }
    assert_log cmake-ninja "greet2.c" 1
    assert_log cmake-ninja "hello.c" 1
}

scenario_autotools() {
    d=$(stage autotools)
    reset_log
    run_build "$d" autoreconf -fi >/dev/null ||
        { echo "FAIL [autotools]: autoreconf failed"; FAIL=1; return; }
    run_build "$d" ./configure >/dev/null ||
        { echo "FAIL [autotools]: configure failed"; FAIL=1; return; }
    run_build "$d" make -s >/dev/null ||
        { echo "FAIL [autotools]: make failed"; FAIL=1; return; }
    assert_log autotools "greet2.c" 1
    assert_log autotools "hello.c" 1
}

scenario_meson() {
    d=$(stage meson)
    reset_log
    run_build "$d" meson setup build >/dev/null ||
        { echo "FAIL [meson]: setup failed"; FAIL=1; return; }
    run_build "$d" ninja -C build >/dev/null ||
        { echo "FAIL [meson]: ninja failed"; FAIL=1; return; }
    assert_log meson "greet2.c" 1
    assert_log meson "hello.c" 1
}

scenario_ninja() {
    d=$(stage ninja)
    reset_log
    run_build "$d" ninja >/dev/null ||
        { echo "FAIL [ninja]: build failed"; FAIL=1; return; }
    assert_log ninja "greet2.c" 1
    assert_log ninja "hello.c" 1
    assert_log ninja "hello.o greet2.o" 1
}

ALL="make cmake-make cmake-ninja autotools meson ninja"
for s in ${@:-$ALL}; do
    echo ""
    echo "== scenario: $s =="
    "scenario_$(printf '%s' "$s" | tr - _)" || FAIL=1
done

echo ""
if [ "$FAIL" -ne 0 ]; then
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
