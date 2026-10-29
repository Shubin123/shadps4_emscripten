#!/usr/bin/env bash
# Translates every GCN shader in a game executable to WGSL (one process per shader), then has
# Chromium's WebGPU compile each result. Prints a summary; details in <out>/report.txt.
#
#   tests/shaders/run_shader_tests.sh <eboot.bin> [out dir]
set -uo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
eboot=$1
out=${2:-$root/build/shaders}
mkdir -p "$out"
rm -f "$out"/*.wgsl
node "$root/build/wasm/translate_shaders.js" "$eboot" "$out" > "$out/list.txt"
n=$(wc -l < "$out/list.txt")
: > "$out/report.txt"
crashed=0
for i in $(seq 0 $((n - 1))); do
  if ! perl -e 'alarm 120; exec @ARGV' node "$root/build/wasm/translate_shaders.js" "$eboot" "$out" "$i" >> "$out/report.txt" 2>"$out/$i.stderr"; then
    if ! grep -q "^$i " "$out/report.txt"; then
      echo "$i CRASH $(grep -m1 -E 'Assertion|Unreachable|RuntimeError|Error' "$out/$i.stderr" | cut -c1-200)" >> "$out/report.txt"
      crashed=$((crashed + 1))
    fi
  fi
done
translated=$(grep -c "errors=0" "$out/report.txt")
partial=$(grep -E "^[0-9]+ (vs|ps|cs) " "$out/report.txt" | grep -vc "errors=0")
echo "shaders: $n, translated cleanly: $translated, with unsupported features: $partial, recompiler crashes: $crashed"
node "$root/tests/shaders/validate_wgsl.mjs" "$out"
