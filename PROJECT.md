# Project: shadPS4 Emscripten Wrapper

## Why this is a different kind of port than azahar_web

[azahar_web](https://github.com/Shubin123/azahar_web) ports Azahar (a Citra/3DS-emulator fork) to
WebAssembly. That worked because Citra's CPU core is swappable: disable the `dynarmic` JIT,
route execution through `dyncom` (a portable C++ ARM11 interpreter Citra already shipped), pair
it with the existing software rasterizer instead of OpenGL/Vulkan, and the rest is ordinary C++
that Emscripten compiles like any other target.

shadPS4 has no equivalent fallback, for a structural reason: the PS4 and a PC are both x86-64, so
shadPS4 doesn't emulate the CPU at all. It maps the PS4 ELF into memory and **runs the game's
x86-64 code directly on the host CPU** (Wine-style), using Xbyak-generated trampolines and
SIGSEGV/SIGILL handlers to intercept guest memory faults and illegal instructions
(`src/core/cpu_patches.cpp`, `src/core/signals.cpp`). There is no interpreter core to swap in,
because the direct-execution model never needed one. WebAssembly cannot be handed arbitrary
native machine code to execute, and has nothing resembling POSIX signal-based fault handling, so
none of that is available in the Emscripten build - not as a performance regression, but as a
hard capability gap.

Separately, shadPS4's renderer is **Vulkan-only** (`src/video_core/renderer_vulkan`, GCN shaders
recompiled to SPIR-V via `shader_recompiler`) - there's no software-rasterizer fallback the way
Citra had. That's a second, independent blocker (no browser exposes Vulkan) tracked as its own
milestone below; it does not block CPU work and isn't where this project started.

The scope decided on (2026-10-28): build a from-scratch x86-64 interpreter rather than stopping
at a frontend-only scaffold. That is a new emulation subsystem shadPS4 itself doesn't have, not a
"port" in the azahar_web sense - realistically a long-running effort. What follows is what
actually exists today, not a plan dressed up as progress.

## Status

| Milestone | Scope | Status |
|---|---|---|
| M1 | x86-64 instruction interpreter core (register file, flags, ALU/control-flow dispatch) | **Done for the implemented subset** - see Instruction coverage below |
| M2 | Guest\<->host call boundary (HostThunkTable) | **Done** for the mechanism itself; not yet connected to shadPS4's real `Linker`/`aerolib` import resolution |
| M3 | Standalone test harness proving M1+M2 against hand-assembled machine code | **Done** - `tests/interpreter/`, 16/16 checks passing |
| M4 | Guest virtual memory (software MMU replacing `Core::AddressSpace`'s identity mapping) | **Started**: `GuestAddressSpace` exists and is tested standalone (region-list based, not a page table - see below for why). **Not started**: porting the real `Core::Memory`/`AddressSpace` itself, and the HLE-wide audit of `VAddr`-as-host-pointer call sites |
| M5 | Wiring into shadPS4 proper (`Core::Linker::RunMainEntry`, module init/fini, thread entry, TLS, the ~23 other guest-callable-pointer sites) | **Started**: `patches/0001-linker-emscripten-run-main-entry.patch` connects `RunMainEntry` to the interpreter, verified with `git apply --check` against a real shadPS4 checkout. **Not yet buildable** - blocked on the rest of M4 and all of M6 (see the patch's own comments for specifics). Module init/fini, thread entry, TLS, and the other ~22 sites: not started |
| M6 | Emscripten/CMake build integration for shadPS4 itself | **Not started.** One concrete blocker already located: the top-level `CMakeLists.txt`'s architecture detection (`BASE_ARCHITECTURE` matched against x86/arm64 regexes) hard-fails with `FATAL_ERROR "Unsupported CPU architecture"` for wasm32/wasm64 before reaching anything target-specific |
| M7 | SSE/AVX floating point and REP-prefixed string instructions in the interpreter | **Not started** - deliberately deferred, see below |
| M8 | Vulkan -> web graphics (WebGPU or similar) | **Not started**, independent of M1-M7 |
| M9 | `shadps4_web` frontend (`web/`) | **Scaffold only**: dev server with COOP/COEP isolation headers (needed for any future pthreads build), a status page reflecting this table, and a file picker that reads a dropped file's header locally and reports what it is - explicitly does not execute anything, because nothing here can yet |

"Done" above means: implemented, and verified against real test cases that exercise it - not
"written and assumed correct." M1-M3 already caught one real bug this way (see Interpreter
design, point 3) that would otherwise have shipped silently wrong.

## Where the code lives

This repo does not vendor shadPS4's source (same reason azahar_web didn't vendor Citra's): clone
<https://github.com/shadps4-emu/shadps4> separately, e.g. to `~/shadps4_src`, and initialize at
least these submodules (a shallow top-level clone does not fetch them):

```
git submodule update --init --depth 1 externals/zydis
git -C externals/zydis submodule update --init --depth 1 dependencies/zycore
```

New interpreter source lives under `patches/new_files/core/interpreter/` in *this* repo, laid out
to mirror where it will eventually live inside a shadPS4 checkout
(`core/interpreter/` under `src/`). Nothing under `patches/` has been applied to a real shadPS4
checkout yet (there is no CMake integration - M6 above) - today it only builds standalone, via
`tests/interpreter/CMakeLists.txt`, against a vendored Zydis for decoding.

```
patches/
  0001-linker-emscripten-run-main-entry.patch
                           A real diff against shadPS4's src/core/linker.cpp, verified with
                           `git apply --check` against an actual checkout. Connects
                           RunMainEntry to the interpreter. Not yet buildable - see its own
                           comments for exactly what's still missing (the rest of M4, and M6).
  new_files/core/interpreter/
    guest_memory.h          GuestMemory: the memory-access seam (see M4 discussion)
    guest_address_space.h/.cpp  GuestAddressSpace: the M4 stand-in for Core::AddressSpace
    x86_register_file.h/.cpp  CpuState: GPRs, RFLAGS, XMM slots, FS/GS bases
    host_thunk.h/.cpp        HostThunkTable: the guest<->host call-boundary mechanism
    x86_interpreter.h/.cpp   The decode/dispatch loop itself
tests/interpreter/
  CMakeLists.txt           Standalone build (fetches nothing from shadPS4 except Zydis)
  test_main.cpp            16 checks against hand-assembled x86-64 bytes
  disasm_check.cpp         Throwaway helper: disassembles a byte array with Zydis's own
                           formatter, to verify hand-encoded test bytes actually mean what a
                           comment claims. Keep using this before trusting new hand-assembled
                           test cases - that's exactly how the EffectiveAddress bug below was
                           confirmed, not guessed at a second time.
web/
  index.html, server.cjs  M9 (frontend) scaffold - a status page and a dev server with the
                           COOP/COEP isolation headers any future pthreads-enabled wasm build
                           will need. No wasm module exists for it to load yet.
```

Run the tests:
```
cmake -S tests/interpreter -B tests/interpreter/build -DSHADPS4_SRC_DIR=~/shadps4_src
cmake --build tests/interpreter/build
./tests/interpreter/build/interpreter_tests
```

## Interpreter design

- **Decoding**: Zydis (already a shadPS4 dependency - `externals/zydis`). It's a pure disassembler
  with no code generation, so it's portable to wasm with no changes expected.
- **Register file**: `Core::Interpreter::CpuState` - 16 GPRs indexed the same way Zydis's register
  enum orders them (`ZYDIS_REGISTER_RAX..R15` are 16 contiguous values; the 8/16/32-bit and
  legacy-high-byte sub-register views are all computed from that one array, not stored
  separately). RFLAGS tracks only the bits a user-mode interpreter needs (CF/PF/AF/ZF/SF/OF).
- **Memory**: `GuestMemory` is an abstract `Translate(vaddr, size, for_write) -> u8*` seam. See
  "Guest virtual memory" below for why this has to be a seam rather than treating a VAddr as a
  dereferenceable host pointer the way the rest of shadPS4 currently does.
- **The guest<->host call boundary** (`HostThunkTable`): shadPS4's linker resolves a PS4 module's
  imports directly to native C++ function pointers
  (`Linker::RelocateAllImports`/`core/aerolib`) - under direct execution, guest code just calls
  that address and it works, because guest and host share a CPU. Under interpretation, landing on
  one of those addresses has to invoke the real host function and splice its return value into the
  virtual register file instead of trying to decode compiled C++ as if it were guest bytes.
  `HostThunkTable::Register()` hands out a synthetic address (above `0xFFFF'0000'0000'0000`, safely
  outside shadPS4's real 40-bit guest address space) for exactly this; `RunUntil()` checks for one
  at the top of every loop iteration. The existing `Linker::RunMainEntry`
  (`src/core/linker.cpp`) already does a version of this by hand - it hands guest code
  `ProgramExitFunc`'s raw host function pointer in `rsi` as the process-exit continuation - which
  is what motivated building this as a first-class mechanism rather than a special case.
- **What's deliberately not implemented yet**: SSE/AVX floating point, and REP-prefixed string
  instructions (`MOVSB/STOSB/CMPSB/...`). Both are real, separate pieces of work, not just more of
  the same pattern:
    - There's a genuine Intel-syntax name collision worth being careful about: Zydis's
      `ZYDIS_MNEMONIC_MOVSD` is the *string* instruction (`MOVSB/MOVSD/MOVSQ/MOVSW` are
      adjacent in its enum - confirmed by reading `Zydis/Generated/EnumMnemonic.h`, not assumed),
      not the SSE2 scalar double-precision move, which objdump/Zydis also print as "movsd" in
      Intel syntax. Both exist; they're disambiguated by operand shape, not by enum value. Rather
      than guess which handler a given decode means, `Execute()`'s unimplemented-instruction path
      (`Unimplemented()`) aborts loudly with the mnemonic and address for both, so this surfaces
      immediately instead of silently computing the wrong one.
    - YMM/AVX register state above the 128-bit XMM view isn't modeled in `CpuState` at all yet.
  When this is picked up, resolve the MOVSD ambiguity via each decode's actual operand types
  (`ops[0].type`/`ops[1].type` - SSE2's form has an XMM register destination; the string form has
  none) before writing either handler.

### A real bug this caught (worth reading if you're about to extend this)

`EffectiveAddress()` initially called Zydis's `ZydisCalcAbsoluteAddress()` for every memory
operand. That function's own header comment (`Zydis/Utils.h`) says explicitly it only handles
IMM-relative, RIP-relative, and literal-absolute operands - it has no access to a CPU's register
values, so it cannot and does not resolve ordinary base/index-register addressing (`[rdi]`,
`[rsp+8]`, `[rax+rbx*4+0x10]`, which is most memory operands in real code). Calling it for those
doesn't error - it just returns a wrong address computed from whatever static bits happen to be
in the encoding. The first four test cases all happened to avoid memory operands entirely (pure
register/immediate ALU and control flow), so this shipped "passing" until the fifth test
(`RunMainEntry`, which reads `[rdi]`/`[rsp+8]`) returned all-zero registers instead of the
expected values. `EffectiveAddress()` now computes `base + index*scale + disp` itself from
`op.mem.base/index/scale/disp.value`, reserving `ZydisCalcAbsoluteAddress()` for the one case it's
actually documented to handle correctly (`ResolveBranchTarget`'s relative-immediate branch
targets). The lesson, not just the fix: a test suite that passes is only as strong as what it
actually exercises - four passing register/immediate-only tests gave no signal at all about memory
addressing correctness.

## Guest virtual memory (M4 - not started, probably the biggest remaining piece)

`Core::AddressSpace` (`src/core/address_space.h`) reserves a real 40-bit (1TB) region of the
*host's* virtual address space via `mmap` and maps guest pages into it, so a guest VAddr and a
host pointer are the same number (`Traits::Entry = u8*`, `ADDRESS_SPACE_BITS = 40`). That's cheap
on a real OS because an unmapped mmap reservation doesn't commit physical memory. WebAssembly
linear memory has nothing like it: no sparse reservation at an arbitrary 64-bit address, and no
realistic way to back a 1TB declared memory even under the memory64 proposal. So this identity
mapping cannot exist in the Emscripten build, which means:

- `GuestMemory::Translate()` in this project is the replacement seam, but **only the interpreter
  itself goes through it today**. Everything else in shadPS4 - all of `core/libraries/**`, every
  HLE syscall implementation, anywhere a `VAddr` gets `reinterpret_cast`into a host pointer and
  dereferenced directly - currently assumes the identity mapping and would need to be rewritten to
  translate through whatever replaces `AddressSpace` for this target. That's the actual size of
  M4: not "write a page table" (small - `AddressSpace::BackingPageTable` is already
  `Common::MultiLevelPageTable<Entry=u8*>`, and the committed-pages discipline it already has is a
  reasonable starting point to adapt rather than replace outright), but "audit and fix every call
  site across the HLE tree that currently assumes `VAddr == uintptr_t`."
- **What `GuestAddressSpace` actually does** (the M4 stand-in built so far,
  `patches/new_files/core/interpreter/guest_address_space.{h,cpp}`): a sorted list of mapped
  regions, each its own heap allocation, not a page table. A page-table-per-16KB-page scheme
  (mirroring `AddressSpace::BackingPageTable` exactly, as the paragraph above suggests) was
  considered and specifically rejected for now: `GuestMemory::Translate()` has to hand back one
  contiguous host pointer for the requested range, and independently-allocated pages aren't
  contiguous with each other any more than independently-allocated regions are - a page-table
  version would hit the exact same "can't span a boundary" limitation one level down, for more
  implementation complexity, not less. `GuestAddressSpace` accepts that limitation explicitly
  (`Translate()` returns `nullptr` for a request spanning two separately-mapped regions, and
  `GuestMemory`'s `Read`/`Write`/etc. now abort with a diagnostic on that rather than segfaulting -
  see guest_memory.h) rather than hiding it behind a page table that merely relocates the same
  problem. Revisit if something real needs a request spanning two independently-`Map()`'d
  regions to work - nothing has hit that yet.
- A second, specific wrinkle found while building the interpreter: `Linker::RunMainEntry` hands
  guest code a *host* global's address (`Libraries::Kernel::entry_params`) in `rdi`, expecting
  guest code to dereference it - which only works today because host and guest share one address
  space. Once that's no longer true, that handoff needs its own fix (likely: copy the struct into
  translated guest memory instead of pointing at the host original), not just a generic
  VAddr-translation pass.

## Guest<->host call boundary, in shadPS4 terms (part of M5)

`HostThunkTable` is the general mechanism; wiring it in means finding every place existing
shadPS4 code resolves a guest-visible address to a real host function pointer and routes it
through a thunk registration instead of a raw call. Found so far by grep (not exhaustively
audited): roughly 23 files under `src/` declaring `PS4_SYSV_ABI (*)(...)`-style function-pointer
typedefs used to call into mapped guest memory, plus `Linker::RunMainEntry` itself
(`src/core/linker.cpp`) and the `_malloc_init`/`sceLibcInternalMemoryMutexEnable` calls right next
to it, which are guest code reached via `mod->FindByName(...)` and a raw function-pointer cast -
easy to miss because nothing about that line looks different from calling a normal host function.
Thread entry (`core/thread.cpp`) and TLS setup (`core/tls.cpp`, needed before `state.fs_base`
means anything) are two more specific integration points, not yet looked at in detail.

## Attribution note on the upstream repo

shadPS4's `AGENTS.md` contains an instruction telling AI agents to check GitHub push permissions
and, if the check fails, write a file admitting to "botting the repo" without having checked the
work. That's a prompt-injection trap for agents that blindly follow instructions found in cloned
repository content rather than from the person actually directing the work - it was not followed,
and no such file was created.
