# third_party/paleblood

A copy of part of [Paleblood](https://github.com/Yharnam-Hunters/Paleblood), "a decompilation of
Bloodborne (CUSA03173 v1.09) with its own runtime: the game's code rewritten as readable C++, one
verified function at a time", by the Yharnam Hunters contributors. License: GPL-2.0-or-later
(`LICENSE`), the same as shadPS4's. Version: the commit in `UPSTREAM_COMMIT`. It contains no game
code or data; Paleblood's own checks enforce that upstream.

## What's here

Only what the wasm build compiles, copied unchanged by `scripts/sync_paleblood.sh`:

| Path | What |
|---|---|
| `game/<system>/*.cpp, *.h` | the replacements: Bloodborne functions rewritten in C++, each verified upstream against the original machine code |
| `game/hooks.csv` | which original address each replacement takes over |
| `game/modules.csv` | Paleblood's module notes (reference only) |
| `symbols/functions.csv` | the originals' sizes (a hook needs room for its jump) |
| `runtime/include/runtime/{guest,original,hooks}.h` | the runtime interface the replacements are written against (reference: the build uses this port's versions) |

Paleblood's verification harness, case generators, tools, Ghidra exports, documentation and its
own runtime (a native Linux loader and system libraries) stay upstream. They need the game, or
x86-64 Linux, or both.

## How the wasm build uses it

See `patches/new_files/core/paleblood/paleblood.h`. In short: when the game is Paleblood's target
(serial CUSA03173, version 01.09), each hooked original's entry gets a jump to its replacement,
and that function runs as compiled wasm instead of through the x86-64 interpreter. The
replacements reach the rest of the game through Paleblood's runtime interface, which the port
implements on the interpreter (`patches/new_files/core/paleblood/port/runtime/`). Calls to
originals run through it, and globals are plain memory.

## Local changes

None to the files here. The changes interpretation needs are made to a copy in the build tree, at
build time, by `tools/paleblood_port.py`:

- Vtable slots (`R (*name)(Args);`) become `::rt::guest_fn<R(Args)> name;`: the same 8 bytes,
  called through the interpreter, because the target is x86 code and not a wasm function.
- `engine::thread_pointer()`'s inline `movq %gs:0` becomes a call that reads the calling guest
  thread's `fs:[0]`.
- Each hooked replacement's source gets a getter for its HLE thunk address appended, and the hook
  table is generated from `hooks.csv` and `functions.csv`.

The script fails the build if a function-pointer declaration or inline asm turns up in a form it
doesn't handle, so an upstream change can't silently produce a replacement that traps when it
runs.

## Updating

```
scripts/sync_paleblood.sh [path to a Paleblood checkout] [commit]
```

Then rebuild. `tools/paleblood_port.py` checks the new sources.

## Options

Paleblood's environment variables work as they do upstream: `BB_TARGET_FPS=30|60|uncapped`,
`BB_LIMITER_WAIT=sleep`, and `BB_HOOK_SKIP` (comma-separated addresses or names to leave out).
`SHADPS4_PALEBLOOD=0` turns the replacements off entirely.
