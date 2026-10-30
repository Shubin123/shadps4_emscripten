# shadPS4 Emscripten port

A WebAssembly build of [shadPS4](https://github.com/shadps4-emu/shadps4), the PS4 emulator. The
browser frontend is [shadps4_web](https://github.com/Shubin123/shadps4_web).

shadPS4 runs PS4 (x86-64) code directly on the host CPU instead of emulating it, which a browser
can't do, so this port adds an x86-64 interpreter and the plumbing to run upstream shadPS4's own
loader, linker, memory manager and HLE libraries around it, compiled for wasm64. Read
[PROJECT.md](PROJECT.md) for the design, status and limits.

**Where it is:** games load and their code runs (in Node and in Chrome), and what they draw
with the PS4 GPU is rendered on WebGPU: shadPS4's shader recompiler emits WGSL, and draws
(graphics pipelines, render targets, depth/stencil, blending, textures) and compute dispatches
run on the GPU, with display buffers read back at each flip. Bloodborne (CUSA03173) boots and
renders through it (every draw and dispatch it issues runs), though slowly: the interpreter does
~60 MIPS. Missing: geometry/tessellation stages, MSAA, and handing GPU results back to the CPU.
Bully (a PS2 Classic, CUSA03507) gets as far as native shadPS4 does: its PS2 emulator
initializes, then stops where it needs a JIT compiler process that shadPS4 doesn't support on
any platform.

**Paleblood:** the build includes the verified C++ rewrites of Bloodborne functions from
[Paleblood](https://github.com/Yharnam-Hunters/Paleblood), the Bloodborne decompilation
(`third_party/paleblood`). When Bloodborne (CUSA03173, version 1.09) runs, those functions run as
compiled wasm instead of through the interpreter. See PROJECT.md, "Paleblood".

## Quick start

```
git clone https://github.com/shadps4-emu/shadps4 ../upstream_native/shadps4
git -C ../upstream_native/shadps4 checkout $(cat UPSTREAM_COMMIT)
git -C ../upstream_native/shadps4 submodule update --init --recursive --depth 1

scripts/setup_tree.sh                    # build/patched = upstream src + patches/
scripts/build_wasm.sh                    # build/wasm/{shadps4,shadps4_node,interpreter_tests,...}.js
node build/wasm/interpreter_tests.js     # interpreter tests
node tests/gpu/run_gpu_tests.mjs         # WebGPU dispatch and draw tests (headless Chromium)

tools/extract_pkg.py GAME.pkg ../games/ps4/GAME   # fake-signed PKGs; needs `cryptography`
scripts/run_node.sh ../games/ps4/GAME/eboot.bin   # headless run, log in build/run/out.txt
                                                  # (renders on Dawn if `webgpu` is installed)
```

For the browser, copy `build/wasm/shadps4.{js,wasm}` into shadps4_web (`scripts/sync_runtime.sh`
there) and serve it with the COOP/COEP headers its dev server sets.
