#!/usr/bin/env bash
# Runs build/native/shadps4 (scripts/build_native.sh): upstream shadPS4 for macOS with Paleblood's
# Bloodborne function replacements. Same options and user data as upstream's: ./user if the
# current directory has one (upstream's portable mode), otherwise
# ~/Library/Application Support/shadPS4.
#
#   scripts/run_native.sh -g ../games/ps4/CUSA03173/eboot.bin
#   SHADPS4_PALEBLOOD=0 scripts/run_native.sh -g ...     # without the replacements
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
exec "$root/build/native/shadps4" "$@"
