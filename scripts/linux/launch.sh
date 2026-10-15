#!/usr/bin/env bash
# Both entry points share the same installation, settings, saves and Wine prefix.
set -euo pipefail
release_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)
fail() {
    printf 'BLACK: %s\n' "$*" >&2
    if command -v zenity >/dev/null 2>&1; then
        zenity --error --title='BLACK launcher' --text="$*" 2>/dev/null || true
    fi
    exit 1
}
case "${1:-play}" in
    play) executable='BLACK PC Launcher.exe' ;;
    settings) executable='BLACK PC Settings.exe' ;;
    *) fail 'Expected play or settings.' ;;
esac
if (( $# )); then shift; fi
[[ -f "$release_root/$executable" ]] || fail 'Launcher is missing. Extract the entire release folder.'
[[ $(uname -m) == x86_64 ]] || fail 'This release requires an x86_64 Linux system with an AVX2 CPU.'
cd -- "$release_root"
# Prefix overrides are explicit; the default never uses ~/.wine.
export WINEPREFIX="${BLACK_WINEPREFIX:-$release_root/.wine}"
[[ "$WINEPREFIX" == /* ]] || fail 'BLACK_WINEPREFIX must be an absolute path.'
if [[ -n ${BLACK_PROTON:-} ]]; then
    [[ -x "$BLACK_PROTON" ]] || fail 'BLACK_PROTON must point to an executable Proton script.'
    [[ -n ${STEAM_COMPAT_CLIENT_INSTALL_PATH:-} ]] || fail 'Set STEAM_COMPAT_CLIENT_INSTALL_PATH to your Steam installation; see docs/linux.md.'
    export STEAM_COMPAT_DATA_PATH="${BLACK_COMPAT_DATA_PATH:-$release_root/.proton}"
    [[ "$STEAM_COMPAT_DATA_PATH" == /* ]] || fail 'BLACK_COMPAT_DATA_PATH must be an absolute path.'
    mkdir -p -- "$STEAM_COMPAT_DATA_PATH"
    exec "$BLACK_PROTON" run "$release_root/$executable" "$@"
fi
wine_command=${BLACK_WINE:-wine}
command -v "$wine_command" >/dev/null 2>&1 || fail 'Wine is missing. Install Wine and its .NET support, or use Steam Proton; see docs/linux.md.'
exec "$wine_command" "$release_root/$executable" "$@"
