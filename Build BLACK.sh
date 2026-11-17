#!/bin/sh
set -eu
release_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
exec bash "$release_root/scripts/setup-linux.sh" "$@"
