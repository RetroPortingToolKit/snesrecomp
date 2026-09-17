#!/usr/bin/env bash
# Build + run the interp816 / interp_bridge validation harnesses.
# Run from anywhere (e.g. under WSL): tests/interp816/run.sh
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
mkdir -p build
# _POSIX_C_SOURCE because -std=c11 is strict ISO: it hides setenv() (phase 1's
# bridge_test) and gmtime_r() (tier2_capture.c), so phase 1 has never compiled
# on Linux. Pre-existing on both main and this branch.
CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter -O1"

# runner/src is organised into layer folders and its sources include each other
# by bare filename, so every layer stays on the search path. Same list as
# SNESRECOMP_RUNNER_INCLUDE_DIRS in runner/runner.cmake.
RUNNER_INC=(-I runner/src -I runner/src/cpu -I runner/src/debug
            -I runner/src/desktop -I runner/src/lobby -I runner/src/mods
            -I runner/src/netplay -I runner/src/state -I runner/src/util
            -I runner/src/snes)

echo "=== Phase 0: interp816 core ==="
gcc $CFLAGS "${RUNNER_INC[@]}" \
    tests/interp816/interp816_test.c runner/src/snes/interp816.c \
    -o build/interp816_test
./build/interp816_test

echo ""
echo "=== Phase 1: interp_bridge contract ==="
gcc $CFLAGS -DSNESRECOMP_TIER2_TEST=1 "${RUNNER_INC[@]}" \
    tests/interp816/bridge_test.c \
    runner/src/snes/interp816.c runner/src/snes/interp_bridge.c \
    runner/src/snes/tier2_capture.c \
    runner/src/snes/cx4.c -lm -o build/bridge_test
exec ./build/bridge_test
