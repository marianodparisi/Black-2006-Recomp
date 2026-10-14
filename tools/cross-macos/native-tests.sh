#!/bin/sh
# Run the portable C tests on macOS as x86_64 binaries under Rosetta 2
# (softwareupdate --install-rosetta). The Windows-only suites (eeprom,
# movie_skip, black_audio_completion, kernel_dpc_queue, apu_pitch,
# apu_spatial, apu_physical_memory, ambient_occlusion) need Windows or Wine.
# _putenv is the Windows CRT name of POSIX putenv.
#
# Known platform failure: ambient_occlusion/camera checks that the guest
# MXCSR comes back unchanged; Rosetta's MXCSR emulation fails it on the
# original upstream source too, so it is not run here.
#
# Usage: tools/cross-macos/native-tests.sh <source dir> <build root>
set -e
SRC="$1"; OUT="$2"; status=0
mkdir -p "$OUT"
for suite in scope_mask viewmodel weapon_trace weapon_model_trace weapon_runtime \
             weapon_model_math kernel_timer_waitset apu_timing apu_output; do
    B="$OUT/$suite"
    rm -rf "$B"
    cmake -S "$SRC/tests/$suite" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_C_COMPILER=/usr/bin/clang \
        "-DCMAKE_C_FLAGS=-D_putenv=putenv" -DPython3_EXECUTABLE="$(command -v python3)" > "$B.log" 2>&1
    cmake --build "$B" >> "$B.log" 2>&1
    if (cd "$B" && ctest --output-on-failure >> "$B.log" 2>&1); then
        echo "pass  $suite"
    else
        echo "FAIL  $suite (see $B.log)"; status=1
    fi
done
exit $status
