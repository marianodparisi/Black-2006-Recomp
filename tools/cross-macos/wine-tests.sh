#!/bin/sh
# Cross-compile the Windows-only C test suites and run them under Wine
# (Gcenx macOS build, https://github.com/Gcenx/macOS_Wine_builds, with Rosetta 2).
# Commands come from CTest itself; each .exe is started through wine.
#
# Usage: tools/cross-macos/wine-tests.sh <source dir> <build root>
# Env: WINE_BIN (dir holding wine), WINEPREFIX, XWIN_DIR.
set -e
SRC="$1"; OUT="$2"; status=0
HERE="$(cd "$(dirname "$0")" && pwd)"
X="${XWIN_DIR:-$HOME/black/toolchain/xwin}"; export XWIN_DIR="$X"
export PATH="${WINE_BIN:-$HOME/black/toolchain/wine/Wine Devel.app/Contents/Resources/wine/bin}:/opt/homebrew/opt/rustup/bin:$PATH"
export WINEPREFIX="${WINEPREFIX:-$HOME/black/toolchain/wineprefix}" WINEDEBUG=-all
export CC_x86_64_pc_windows_msvc=/opt/homebrew/opt/llvm/bin/clang-cl
export AR_x86_64_pc_windows_msvc=/opt/homebrew/opt/llvm/bin/llvm-lib
export CFLAGS_x86_64_pc_windows_msvc="--target=x86_64-pc-windows-msvc /imsvc$X/crt/include /imsvc$X/sdk/include/ucrt /imsvc$X/sdk/include/um /imsvc$X/sdk/include/shared"
mkdir -p "$OUT"
for suite in eeprom movie_skip black_audio_completion kernel_dpc_queue apu_pitch apu_spatial apu_physical_memory ambient_occlusion; do
    B="$OUT/$suite"
    rm -rf "$B"
    if ! { cmake -S "$SRC/tests/$suite" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_TOOLCHAIN_FILE="$HERE/clang-cl-xwin.cmake" \
             -DPython3_EXECUTABLE="$(command -v python3)" -DAO_POWERSHELL=/usr/bin/false \
             && cmake --build "$B"; } > "$B.log" 2>&1; then
        echo "NOBUILD $suite (see $B.log)"; status=1; continue
    fi
    (cd "$B" && ctest --show-only=json-v1) > "$B.tests.json"
    python3 - "$B" "$B.tests.json" <<'PY' || status=1
import json, subprocess, sys
build, listing = sys.argv[1], sys.argv[2]
failed = 0
for test in json.load(open(listing))["tests"]:
    command = test["command"]
    if test["name"] == "ao-launcher":
        print("skip  ao-launcher (PowerShell/.NET launcher suite; see the mono notes)")
        continue
    env = dict(item.split("=", 1) for p in test.get("properties", [])
               if p["name"] == "ENVIRONMENT" for item in p["value"])
    cwd = next((p["value"] for p in test.get("properties", []) if p["name"] == "WORKING_DIRECTORY"), build)
    import os
    launcher = ["wine"] if command[0].lower().endswith(".exe") else []
    run = subprocess.run([*launcher, *command], cwd=cwd, env={**os.environ, **env},
                         capture_output=True, text=True, errors="replace", timeout=600)
    will_fail = any(p["name"] == "WILL_FAIL" and p["value"] for p in test.get("properties", []))
    ok = (run.returncode != 0) if will_fail else (run.returncode == 0)
    failed += not ok
    print(("pass  " if ok else "FAIL  ") + test["name"] + ("  (expected failure)" if will_fail and ok else ""))
    if not ok:
        print((run.stdout + run.stderr)[-1500:])
sys.exit(1 if failed else 0)
PY
done
exit $status
