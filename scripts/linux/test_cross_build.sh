#!/usr/bin/env bash
# Run the asset-free production runtime/FFI link check under Wine.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
tool_root="$project_root/.work/linux-toolchain"
export CARGO_HOME="$tool_root/cargo"
export RUSTUP_HOME="$tool_root/rustup"
export RUSTUP_TOOLCHAIN=1.98.1
export PATH="$CARGO_HOME/bin:$tool_root/bin:/usr/lib/llvm-18/bin:$PATH"
export BLACK_XWIN_ROOT="$tool_root/xwin"
export CC_x86_64_pc_windows_msvc=clang-cl
export AR_x86_64_pc_windows_msvc=llvm-lib
export CC_SHELL_ESCAPED_FLAGS=1
export CFLAGS_x86_64_pc_windows_msvc="/imsvc\"$BLACK_XWIN_ROOT/crt/include\" /imsvc\"$BLACK_XWIN_ROOT/sdk/include/ucrt\" /imsvc\"$BLACK_XWIN_ROOT/sdk/include/shared\" /imsvc\"$BLACK_XWIN_ROOT/sdk/include/um\""
build="$project_root/build-linux-cross-test"
cmake -S "$project_root/tests/linux_build" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$project_root/cmake/linux-windows-clang.cmake" \
    -DCARGO_EXECUTABLE="$CARGO_HOME/bin/cargo"
cmake --build "$build" --target linux-cross-runtime-test --parallel 2
export WINEPREFIX="$project_root/.work/linux-cross-test-prefix"
export WINEDEBUG=-all
wine "$build/linux-cross-runtime-test.exe"
