#!/usr/bin/env bash
set -euo pipefail
directory="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$directory/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$directory/galdrun-core" "$@"
