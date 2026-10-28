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
  file, flags, a growing integer/control-flow instruction set, and the mechanism for guest code to
  call back out to real host functions (`HostThunkTable`) - not yet wired into shadPS4 itself.
- `tests/interpreter/` - a standalone build proving the above against hand-assembled machine
  code, independent of the (not yet written) Emscripten/shadPS4 build integration:

  ```
  git clone https://github.com/shadps4-emu/shadps4 ~/shadps4_src
  git -C ~/shadps4_src submodule update --init --depth 1 externals/zydis
  git -C ~/shadps4_src/externals/zydis submodule update --init --depth 1 dependencies/zycore

  cmake -S tests/interpreter -B tests/interpreter/build -DSHADPS4_SRC_DIR=~/shadps4_src
  cmake --build tests/interpreter/build
  ./tests/interpreter/build/interpreter_tests
  ```

See [PROJECT.md](PROJECT.md) for the full architecture, what's actually implemented vs. planned,
and the two big pieces of remaining work (guest virtual memory, and wiring the interpreter into
shadPS4's linker/HLE layers) in detail.
