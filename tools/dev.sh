#!/usr/bin/env bash
# Developer loop for Gunmetal RE: configure+build, fast tests, full suite.
#
#   tools/dev.sh build            configure+build build/dev (Debug)
#   tools/dev.sh test             build/test (RelWithDebInfo) + all CTest tests
#   tools/dev.sh quick            build/dev + CTest label "quick" (no SBV/parity)
#   tools/dev.sh full             build/dev + all CTest tests + regress.py
#   tools/dev.sh regress [--full] python3 regress.py (80 / 84 checks)
#   tools/dev.sh sbv [--quick]    the SBV frame-hash test on build/dev
#
# Every command prints its own wall-clock time.  No test is skipped silently:
# "quick" is a labelled subset of the very same CTest tests, and "full" runs the
# whole suite plus the Python regression.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo"

# Toolchain from ~/tools (see docs/BUILD.md); harmless when it is not there.
export PATH="$HOME/tools/cmake-4.4.4-linux-x86_64/bin:$HOME/tools/bin:$PATH"

jobs=${GM_JOBS:-$(nproc 2>/dev/null || echo 4)}
py=${PYTHON:-python3}

say() { printf '\n== %s\n' "$*" >&2; }

# Configure only when the build directory does not exist yet; `cmake --build`
# re-runs CMake itself when a CMakeLists.txt changed.
ensure() {
    local preset=$1
    local t0=$SECONDS
    if [ ! -f "build/$preset/CMakeCache.txt" ]; then
        say "configure: $preset"
        cmake --preset "$preset"
    fi
    say "build: $preset (-j $jobs)"
    cmake --build --preset "$preset" -j "$jobs"
    say "build: $preset done in $((SECONDS - t0))s"
}

usage() {
    sed -n '2,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-1}"
}

cmd=${1:-help}
[ $# -gt 0 ] && shift || true

case "$cmd" in
build)
    ensure "${1:-dev}"
    ;;
test)
    # RelWithDebInfo: identical tests and goldens, ~4x faster than Debug.
    ensure test
    say "ctest: all tests (-j 8)"
    ctest --preset test
    ;;
quick)
    ensure dev
    say "ctest: label quick (-j 8)"
    ctest --preset quick
    ;;
full)
    ensure dev
    say "ctest: all tests (-j 8)"
    ctest --preset dev
    say "regress.py"
    "$py" regress.py
    ;;
regress)
    say "regress.py $*"
    "$py" regress.py "$@"
    ;;
sbv)
    # Default is the full 500-frame sweep; --quick samples every 25th golden.
    ensure dev
    "build/dev/engine/tests/gm_sbv_video_tests" "$@"
    ;;
help | -h | --help)
    usage 0
    ;;
*)
    printf 'unknown command: %s\n' "$cmd" >&2
    usage 1
    ;;
esac
