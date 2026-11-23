#!/usr/bin/env bash
# Generate and build on Linux using native compilers. Wine is only for play.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
# --game-only: the launcher builds the game with it (and is running, so it is not rebuilt)
prepare_only=0 skip_generate=0 game_only=0
for argument in "$@"; do
    case "$argument" in
        --prepare-only) prepare_only=1 ;;
        --skip-generate) skip_generate=1 ;;
        --game-only) game_only=1 ;;
        *) printf 'Usage: %s [--prepare-only] [--skip-generate] [--game-only]\n' "$0" >&2; exit 1 ;;
    esac
done
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || { echo 'Use an x86_64 Linux host.' >&2; exit 1; }
# Ubuntu keeps clang-cl and several LLVM utilities in its versioned bin folder.
if [[ -d /usr/lib/llvm-18/bin ]]; then export PATH="/usr/lib/llvm-18/bin:$PATH"; fi
for tool in python3 git cmake ninja clang-cl lld-link llvm-lib llvm-rc llvm-mt cc c++; do
    command -v "$tool" >/dev/null || { echo "Missing $tool; see docs/linux.md for Linux build dependencies." >&2; exit 1; }
done
# The native launcher (launcher/, C++ on Walnut), copied to $1. It needs the X11/Wayland development packages
# listed in docs/linux.md, and the Vulkan loader every graphics driver installs.
build_launcher() {
    if [[ ! -f "$project_root/launcher/CMakeLists.txt" && -x "$1/BLACK Launcher" ]]; then
        return  # a release archive has the launcher already built, and not its sources
    fi
    if [[ -e "$project_root/.git" && ! -d "$project_root/launcher/vendor/walnut/Walnut" ]]; then
        git -C "$project_root" submodule update --init --recursive
    fi
    (cd "$project_root/launcher" && cmake --preset linux-release && cmake --build --preset linux-release --target black_launcher)
    mkdir -p "$1"
    cp "$project_root/build-launcher-linux/bin/BLACK Launcher" "$1/BLACK Launcher"
}
if (( ! prepare_only && ! game_only )); then
    build_launcher "$project_root"   # first: the disc is set up from the launcher
    if [[ ! -f "$project_root/game/default.xbe" ]]; then
        printf 'Next: open %s/BLACK Settings.sh, choose your disc, then run Build BLACK.sh again.\n' "$project_root"
        exit 0
    fi
fi
if (( ! prepare_only )); then
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
dsp="$project_root/third_party/dsp56300"
# XboxRecomp (the project fork named in .gitmodules) and the DSP56300 core are submodules at the commits this
# checkout records. Only missing ones are fetched, so a checked-out runtime branch with new commits is left alone.
# A release archive without .git ships them already in place.
if [[ -e "$project_root/.git" && ( ! -f "$toolkit/CMakeLists.txt" || ! -f "$dsp/crates/emu-ffi/Cargo.toml" ) ]]; then
    git -C "$project_root" submodule update --init --recursive
fi
for required in "$toolkit/CMakeLists.txt" "$dsp/crates/emu-ffi/Cargo.toml"; do
    [[ -f "$required" ]] || { echo "$required is missing. Clone with --recursive, or run: git submodule update --init --recursive" >&2; exit 1; }
done
if [[ ! -x "$project_root/.venv/bin/python" ]]; then python3 -m venv "$project_root/.venv"; fi
python="$project_root/.venv/bin/python"
"$python" -m pip install --no-cache-dir capstone==5.0.7
if (( prepare_only )); then
    build_launcher "$project_root/build-release-launchers"
    exit 0
fi
if (( ! skip_generate )); then "$python" "$project_root/scripts/generate.py"; fi
build="$project_root/build-linux-wine"
cmake -S "$project_root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$project_root/cmake/linux-windows-clang.cmake" \
    -DDSP56300_SOURCE_DIR="$dsp" -DCARGO_EXECUTABLE="$CARGO_HOME/bin/cargo"
jobs=${BLACK_BUILD_JOBS:-2}
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo 'BLACK_BUILD_JOBS must be a positive integer.' >&2; exit 1; }
cmake --build "$build" --target black_xbox_recomp --parallel "$jobs"
mkdir -p "$project_root/bin"
cp "$build/black_xbox_recomp.exe" "$project_root/bin/black_xbox_recomp.exe"
"$python" "$project_root/scripts/linux/write_build_provenance.py"
printf 'Ready: %s/Play BLACK.sh\nSettings: %s/BLACK Settings.sh\n' "$project_root" "$project_root"
