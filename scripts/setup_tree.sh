#!/usr/bin/env bash
# Creates build/patched: upstream shadPS4's src/ at the pinned commit, as its own git repo (one
# "upstream" commit), with patches/*.patch applied on top as uncommitted changes. Edit files there
# while developing, then run scripts/export_patches.sh to turn the diff back into patches/.
#
#   SHADPS4_SRC_DIR=../upstream_native/shadps4 scripts/setup_tree.sh [--force]
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
src=${SHADPS4_SRC_DIR:-$root/../upstream_native/shadps4}
pin=$(cat "$root/UPSTREAM_COMMIT")
dest="$root/build/patched"

if [ "${1:-}" = "--force" ]; then
  rm -rf "$dest"
fi
if [ -d "$dest/.git" ]; then
  echo "build/patched already exists (pass --force to recreate it from scratch)"
  exit 0
fi

git -C "$src" cat-file -e "$pin^{commit}" 2>/dev/null || {
  echo "commit $pin not in $src - fetch it there first" >&2
  exit 1
}

mkdir -p "$dest"
git -C "$src" archive "$pin" src | tar -x -C "$dest"
git -C "$dest" init -q
git -C "$dest" add -A
git -C "$dest" -c user.name=upstream -c user.email=upstream@localhost commit -q -m "upstream src @ $pin"
git -C "$dest" tag upstream

for p in "$root"/patches/*.patch; do
  [ -e "$p" ] || continue
  echo "applying $(basename "$p")"
  git -C "$dest" apply --whitespace=nowarn "$p"
done
