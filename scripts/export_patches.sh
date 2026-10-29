#!/usr/bin/env bash
# Writes the working-tree diff of build/patched (against its "upstream" tag) back into
# patches/0001-emscripten.patch. New files live in patches/new_files instead and are not part of
# the diff - the wasm build compiles them from there directly.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
dest="$root/build/patched"
out="$root/patches/0001-emscripten.patch"

git -C "$dest" diff upstream -- src > "$out"
echo "wrote $(grep -c '^diff --git' "$out") files to $(basename "$out")"
