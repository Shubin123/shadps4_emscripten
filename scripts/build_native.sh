#!/usr/bin/env bash
# Builds upstream shadPS4 for macOS (x86_64, run under Rosetta 2) with Paleblood's Bloodborne
# function replacements compiled in (native/paleblood.cmake), into build/native. The shadPS4
# checkout isn't modified. Upstream bundles its own build of KosmicKrisp, Mesa's Vulkan driver;
# that needs Mesa's toolchain, so instead the driver is copied from upstream's official macOS build
# of the same commit.
#
#   SHADPS4_SRC_DIR=../upstream_native/shadps4 SHADPS4_VULKAN_DIR=../upstream_native/shadps4-macos \
#     scripts/build_native.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
src=${SHADPS4_SRC_DIR:-$root/../upstream_native/shadps4}
vulkan=${SHADPS4_VULKAN_DIR:-$root/../upstream_native/shadps4-macos}
pin=$(cat "$root/UPSTREAM_COMMIT")
out="$root/build/native"

head=$(git -C "$src" rev-parse HEAD)
if [ "$head" != "$pin" ]; then
  echo "warning: $src is at $head, not the pinned $pin" >&2
fi

cmake -S "$src" -B "$out" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DENABLE_SYSTEM_VULKAN=ON \
  -DCMAKE_BUILD_RPATH=@executable_path \
  -DCMAKE_PROJECT_shadPS4_INCLUDE="$root/native/paleblood.cmake"
cmake --build "$out" --parallel "$(sysctl -n hw.ncpu)"

# shadPS4 points the Vulkan loader at the executable's directory for the driver.
for f in libvulkan.dylib libvulkan_kosmickrisp.dylib kosmickrisp_mesa_icd.json; do
  cp -p "$vulkan/$f" "$out/$f"
done
echo "built $out/shadps4"
