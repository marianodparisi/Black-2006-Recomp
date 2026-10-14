#!/bin/sh
# Cross-compile the Windows x64 build on macOS with clang-cl, lld-link and xwin.
# Compile check only: the result runs on Windows, not here.
#
# One-time setup:
#   brew install llvm lld cmake ninja xwin rustup
#   rustup toolchain install 1.98.1 --profile minimal --target x86_64-pc-windows-msvc
#   xwin --accept-license --arch x86_64 splat --output "$XWIN_DIR"   (Microsoft SDK license)
#   git clone --depth 1 --branch v1.0.0 https://github.com/mborgerson/dsp56300.git .work/err009-dsp56300
#   third_party/xboxrecomp at the pinned commit with patches/xboxrecomp.patch applied,
#   then tools/cross-macos/xboxrecomp-clang-cl.patch on top of it;
#   src/recomp/gen from scripts/generate.py.
# Build from a separate worktree that also has repo-clang-cl.patch applied, so
# this branch stays clean: both patches only fix MSVC-tolerated issues clang rejects.
#
# Usage: tools/cross-macos/build.sh <source dir> <build dir> [ninja args]
set -e
SRC="$1"; B="$2"; shift 2
HERE="$(cd "$(dirname "$0")" && pwd)"
X="${XWIN_DIR:-$HOME/black/toolchain/xwin}"; export XWIN_DIR="$X"
export PATH=/opt/homebrew/opt/rustup/bin:$PATH
export CC_x86_64_pc_windows_msvc=/opt/homebrew/opt/llvm/bin/clang-cl
export AR_x86_64_pc_windows_msvc=/opt/homebrew/opt/llvm/bin/llvm-lib
export CFLAGS_x86_64_pc_windows_msvc="--target=x86_64-pc-windows-msvc /imsvc$X/crt/include /imsvc$X/sdk/include/ucrt /imsvc$X/sdk/include/um /imsvc$X/sdk/include/shared"
[ -f "$B/build.ninja" ] || cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$HERE/clang-cl-xwin.cmake" -DCMAKE_BUILD_TYPE=Release
cmake --build "$B" -- "$@"
