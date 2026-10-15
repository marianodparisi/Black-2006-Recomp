#!/usr/bin/env bash
set -euo pipefail
release_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
python3 "$release_root/scripts/linux/install-shortcuts.py"
