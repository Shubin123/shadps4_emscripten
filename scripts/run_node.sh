#!/usr/bin/env bash
# Runs the Node build of the wasm port headlessly on a game folder/eboot, for up to N seconds,
# with the emulator's user directory in a scratch folder. Output goes to stdout and $RUN_DIR.
#
#   scripts/run_node.sh <eboot.bin or game dir> [seconds=120]
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
game=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
secs=${2:-120}
run_dir=${RUN_DIR:-$root/build/run}
mkdir -p "$run_dir/user" "$run_dir/frames"
cd "$run_dir"
SHADPS4_FRAME_DUMP="$run_dir/frames" node "$root/build/wasm/shadps4_node.js" "$game" > "$run_dir/out.txt" 2>&1 &
pid=$!
for _ in $(seq 1 "$secs"); do
  kill -0 $pid 2>/dev/null || break
  sleep 1
done
if kill -0 $pid 2>/dev/null; then
  echo "[run_node] still running after ${secs}s, stopping it"
  kill $pid
fi
wait $pid 2>/dev/null
echo "[run_node] log: $run_dir/out.txt ($(wc -l < "$run_dir/out.txt") lines)"
