#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
command -v python3 >/dev/null 2>&1 || { echo 'Release requires python3.' >&2; exit 1; }
exec python3 tools/release.py "$@"
