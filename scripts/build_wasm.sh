#!/usr/bin/env bash
# Configures and builds the wasm target. Run scripts/setup_tree.sh once first.
#
#   scripts/build_wasm.sh [extra cmake --build args...]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build="${BUILD_DIR:-$root/build/wasm}"

# Homebrew's em++ wrapper exports this, but CMake's toolchain file calls the unwrapped
# libexec/em++, which then falls back to whatever python3 is first on PATH (Xcode's 3.9 - too old).
if [ -z "${EMSDK_PYTHON:-}" ]; then
  for py in /opt/homebrew/opt/python@3.15/bin/python3.15 /opt/homebrew/bin/python3.12 python3; do
    if command -v "$py" >/dev/null && "$py" -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
      export EMSDK_PYTHON=$(command -v "$py")
      break
    fi
  done
fi

if [ ! -f "$build/build.ninja" ]; then
  emcmake cmake -S "$root/emscripten" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE="${BUILD_TYPE:-Release}" \
    -DSHADPS4_SRC_DIR="${SHADPS4_SRC_DIR:-$root/../upstream_native/shadps4}"
fi
cmake --build "$build" "$@"
