# shadPS4 Emscripten port

A WebAssembly build of [shadPS4](https://github.com/shadps4-emu/shadps4), the PS4 emulator. The
browser frontend is [shadps4_web](https://github.com/Shubin123/shadps4_web).

shadPS4 runs PS4 (x86-64) code directly on the host CPU instead of emulating it, which a browser
can't do, so this port adds an x86-64 interpreter and the plumbing to run upstream shadPS4's own
loader, linker, memory manager and HLE libraries around it, compiled for wasm64. Read
[PROJECT.md](PROJECT.md) for the design, status and limits.

**Where it is:** games load and their code runs (in Node and in Chrome), and the WebGPU backend
has started: shadPS4's shader recompiler emits WGSL, and guest compute dispatches run on WebGPU.
Nothing the PS4 GPU draws is rendered yet, because draws aren't implemented, and the interpreter
is slow (~60 MIPS). The test game, Bully (a PS2 Classic, CUSA03507), gets as far as
native shadPS4 does: its PS2 emulator initializes, then stops where it needs a JIT compiler
process that shadPS4 doesn't support on any platform.

## Quick start

```
git clone https://github.com/shadps4-emu/shadps4 ~/shadps4_src
git -C ~/shadps4_src checkout $(cat UPSTREAM_COMMIT)
git -C ~/shadps4_src submodule update --init --recursive --depth 1

scripts/setup_tree.sh                    # build/patched = upstream src + patches/
scripts/build_wasm.sh                    # build/wasm/{shadps4,shadps4_node,interpreter_tests,...}.js
node build/wasm/interpreter_tests.js     # interpreter tests
node tests/gpu/run_gpu_tests.mjs         # WebGPU compute tests (headless Chromium via Playwright)

tools/extract_pkg.py GAME.pkg ../games/ps4/GAME   # fake-signed PKGs; needs `cryptography`
scripts/run_node.sh ../games/ps4/GAME/eboot.bin   # headless run, log in build/run/out.txt
```

For the browser, copy `build/wasm/shadps4.{js,wasm}` into shadps4_web (`scripts/sync_runtime.sh`
there) and serve it with the COOP/COEP headers its dev server sets.
