#!/usr/bin/env bash
# Copies the parts of Paleblood (https://github.com/Yharnam-Hunters/Paleblood, GPL-2.0-or-later)
# that the wasm build compiles into third_party/paleblood: the game function replacements
# (game/), their hook registry, the function sizes and the runtime interface headers they're
# written against. Paleblood's tools, verification cases and Ghidra exports stay upstream.
#
#   scripts/sync_paleblood.sh [paleblood checkout] [commit]
#
# Without a checkout, clones the repository into a temporary directory. The copy is pristine;
# the changes the wasm build needs are made at build time by tools/paleblood_port.py.
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
dest="$here/third_party/paleblood"
src=${1:-}
commit=${2:-}
tmp=""
if [ -z "$src" ]; then
    tmp=$(mktemp -d)
    git clone -q https://github.com/Yharnam-Hunters/Paleblood.git "$tmp/Paleblood"
    src="$tmp/Paleblood"
fi
if [ -n "$commit" ]; then
    git -C "$src" checkout -q "$commit"
fi
rev=$(git -C "$src" rev-parse HEAD)

rm -rf "$dest/game" "$dest/runtime" "$dest/symbols"
mkdir -p "$dest/game" "$dest/runtime/include/runtime" "$dest/symbols"
(cd "$src/game" && find . -name '*.cpp' -o -name '*.c' -o -name '*.h' -o -name 'hooks.csv' \
    -o -name 'modules.csv' | sort | while read -r f; do
        mkdir -p "$dest/game/$(dirname "$f")"
        cp "$f" "$dest/game/$f"
    done)
for h in guest.h original.h hooks.h; do
    cp "$src/runtime/include/runtime/$h" "$dest/runtime/include/runtime/$h"
done
cp "$src/symbols/functions.csv" "$dest/symbols/functions.csv"
cp "$src/LICENSE" "$dest/LICENSE"
echo "$rev" > "$dest/UPSTREAM_COMMIT"
[ -n "$tmp" ] && rm -rf "$tmp"
echo "third_party/paleblood <- Paleblood $rev"
