#!/usr/bin/env bash
set -euo pipefail
release_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
exec bash "$release_root/scripts/setup-linux.sh" "$@"
