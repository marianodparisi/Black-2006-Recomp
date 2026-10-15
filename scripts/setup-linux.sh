#!/usr/bin/env bash
# Generate and build on Linux using native compilers. Wine is only for play.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
mode=${1:-build}
case "$mode" in
    build|--prepare-only|--skip-generate) ;;
    *) printf 'Usage: %s [--prepare-only|--skip-generate]\n' "$0" >&2; exit 1 ;;
esac
[[ $# -le 1 ]] || { echo 'Unexpected arguments.' >&2; exit 1; }
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'Use an x86_64 Linux host.' >&2; exit 1; }
# Ubuntu keeps clang-cl and several LLVM utilities in its versioned bin folder.
if [[ -d /usr/lib/llvm-18/bin ]]; then export PATH="/usr/lib/llvm-18/bin:$PATH"; fi
for tool in python3 git cmake ninja clang-cl lld-link llvm-lib llvm-rc llvm-mt mcs cc; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; see docs/linux.md for Linux build dependencies." >&2; exit 1; }
done
if [[ "$mode" != --prepare-only ]]; then
    [[ -f "$project_root/game/default.xbe" ]] || { echo 'Supply your original Xbox files under game/ first.' >&2; exit 1; }
    printf '%s  %s\n' df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6 "$project_root/game/default.xbe" | sha256sum --check
fi
tool_root="$project_root/.work/linux-toolchain"
python3 "$project_root/scripts/linux/bootstrap_toolchain.py"
export CARGO_HOME="$tool_root/cargo"
export RUSTUP_HOME="$tool_root/rustup"
export RUSTUP_TOOLCHAIN=1.98.1
export PATH="$CARGO_HOME/bin:$tool_root/bin:$PATH"
export BLACK_XWIN_ROOT="$tool_root/xwin"
export CC_x86_64_pc_windows_msvc=clang-cl
export AR_x86_64_pc_windows_msvc=llvm-lib
export CC_SHELL_ESCAPED_FLAGS=1
export CFLAGS_x86_64_pc_windows_msvc="/imsvc\"$BLACK_XWIN_ROOT/crt/include\" /imsvc\"$BLACK_XWIN_ROOT/sdk/include/ucrt\" /imsvc\"$BLACK_XWIN_ROOT/sdk/include/shared\" /imsvc\"$BLACK_XWIN_ROOT/sdk/include/um\""
toolkit="$project_root/third_party/xboxrecomp"
dsp="$project_root/.work/err009-dsp56300"
if [[ ! -e "$toolkit" ]]; then
    mkdir -p "$project_root/third_party"
    git clone https://github.com/sp00nznet/xboxrecomp.git "$toolkit"
    git -C "$toolkit" checkout --detach 766ecefcd7fb2a9b344de8ec891f6fe9ea14261b
fi
[[ $(git -C "$toolkit" rev-parse HEAD) == 766ecefcd7fb2a9b344de8ec891f6fe9ea14261b ]] || { echo 'Runtime checkout differs from the pinned revision; preserve it separately.' >&2; exit 1; }
for patch in xboxrecomp.patch xboxrecomp-clang.patch; do
    if git -C "$toolkit" apply --check "$project_root/patches/$patch" 2>/dev/null; then
        git -C "$toolkit" apply "$project_root/patches/$patch"
    else
        git -C "$toolkit" apply --reverse --check "$project_root/patches/$patch" || { echo "Conflicting $patch; preserve dependency edits before retrying." >&2; exit 1; }
    fi
done
if [[ ! -e "$dsp" ]]; then
    git clone --depth 1 --branch v1.0.0 https://github.com/mborgerson/dsp56300.git "$dsp"
fi
[[ $(git -C "$dsp" rev-parse HEAD) == aab3649fc452daf03da8b5e36fdc0c308d874101 ]] || { echo 'DSP source differs from the pinned revision; preserve it separately.' >&2; exit 1; }
if [[ ! -x "$project_root/.venv/bin/python" ]]; then python3 -m venv "$project_root/.venv"; fi
python="$project_root/.venv/bin/python"
"$python" -m pip install capstone==5.0.7
if [[ "$mode" == --prepare-only ]]; then
    "$python" "$project_root/scripts/linux/build_launcher.py" --output "$project_root/build-release-launchers"
    exit 0
fi
if [[ "$mode" != --skip-generate ]]; then "$python" "$project_root/scripts/generate.py"; fi
build="$project_root/build-linux-wine"
cmake -S "$project_root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$project_root/cmake/linux-windows-clang.cmake" \
    -DDSP56300_SOURCE_DIR="$dsp" -DCARGO_EXECUTABLE="$CARGO_HOME/bin/cargo"
jobs=${BLACK_BUILD_JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'BLACK_BUILD_JOBS must be a positive integer.' >&2; exit 1; }
cmake --build "$build" --target black_xbox_recomp --parallel "$jobs"
mkdir -p "$project_root/bin"
cp "$build/black_xbox_recomp.exe" "$project_root/bin/black_xbox_recomp.exe"
"$python" "$project_root/scripts/linux/build_launcher.py"
"$python" "$project_root/scripts/linux/write_build_provenance.py"
printf 'Ready: %s/Play BLACK.sh\nSettings: %s/BLACK Settings.sh\n' "$project_root" "$project_root"
