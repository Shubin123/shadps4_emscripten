# shadPS4 Emscripten wrapper

An attempt at a WebAssembly build of [shadPS4](https://github.com/shadps4-emu/shadps4), in the
spirit of [azahar_web](https://github.com/Shubin123/azahar_web) (the same approach applied to
Azahar/Citra). It is **not** at the point of running anything yet - read
[PROJECT.md](PROJECT.md) first. The short version: shadPS4 runs PS4 (x86-64) game code directly
on the host CPU rather than emulating it, which is not something WebAssembly can do at all, so
unlike the Citra port this isn't a matter of swapping in an existing interpreter - it means
building one from scratch. That's what's in progress.

## What exists right now

- `patches/new_files/core/interpreter/` - a from-scratch x86-64 instruction interpreter: register
  file, flags, a growing integer/control-flow instruction set, a guest virtual-memory stand-in
  (`GuestAddressSpace`), and the mechanism for guest code to call back out to real host functions
  (`HostThunkTable`).
- `patches/0001-linker-emscripten-run-main-entry.patch` - a real diff against shadPS4's
  `src/core/linker.cpp` connecting its `RunMainEntry` to the interpreter above. Verified to apply
  cleanly; not yet buildable (see PROJECT.md for exactly what's still missing).
- `tests/interpreter/` - a standalone build proving the interpreter and `GuestAddressSpace`
  against hand-assembled machine code, independent of the (not yet written) Emscripten/shadPS4
  build integration:

  ```
  git clone https://github.com/shadps4-emu/shadps4 ~/shadps4_src
  git -C ~/shadps4_src submodule update --init --depth 1 externals/zydis
  git -C ~/shadps4_src/externals/zydis submodule update --init --depth 1 dependencies/zycore

  cmake -S tests/interpreter -B tests/interpreter/build -DSHADPS4_SRC_DIR=~/shadps4_src
  cmake --build tests/interpreter/build
  ./tests/interpreter/build/interpreter_tests
  ```
- The web frontend lives in a separate repo,
  [shadps4_web](https://github.com/Shubin123/shadps4_web) (live at
  <https://shubin123.github.io/shadps4_web/>). That repo holds only the static site and, once
  this backend produces one, the built `.wasm`/`.js` artifacts - the same split as Azahar's
  engine checkout vs. [azahar_web](https://github.com/Shubin123/azahar_web).

See [PROJECT.md](PROJECT.md) for the full architecture, what's actually implemented vs. planned,
and the two big pieces of remaining work (guest virtual memory, and wiring the interpreter into
shadPS4's linker/HLE layers) in detail.
