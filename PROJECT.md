# Project: shadPS4 Emscripten port

## Why this is a different kind of port than azahar_web

[azahar_web](https://github.com/Shubin123/azahar_web) ports Azahar (a Citra fork) to WebAssembly
by swapping Citra's JIT for the portable ARM interpreter Citra already shipped. shadPS4 has no
equivalent: the PS4 and a PC are both x86-64, so shadPS4 doesn't emulate the CPU at all - it maps
the PS4 ELF into memory and **runs the game's x86-64 code directly on the host CPU**, patching
what the host can't run (`core/cpu_patches.cpp`) and catching faults with signal handlers
(`core/signals.cpp`). WebAssembly can't execute machine code it's handed and has no signal-based
fault handling, so this port needed a new CPU interpreter, a different guest-memory scheme, and a
way for interpreted guest code and shadPS4's C++ to call each other. Separately, shadPS4's
renderer is Vulkan-only (`video_core/renderer_vulkan`, GCN shaders recompiled to SPIR-V); no
browser exposes Vulkan.

## Status

| Milestone | Scope | Status |
|---|---|---|
| M1 | x86-64 interpreter | **Done for what the PS4's Jaguar implements**: integer, x87, SSE-SSE4.2, AVX (no AVX2/FMA, as on Jaguar), F16C, BMI1, LZCNT/POPCNT, MOVBE, AES-NI, PCLMULQDQ. ~60 MIPS on an Apple M4 (see Performance) |
| M2 | Guest -> host calls (HLE) | **Done**: every `LIB_FUNCTION` gets a synthetic address; its SysV-ABI argument marshalling is generated from the C++ signature (`interpreter/hle.h`), including variadic functions via `VA_CTX` |
| M3 | Tests | **Done**: 22 checks against assembler-generated x86-64 (`tests/interpreter`), run under Node |
| M4 | Guest virtual memory | **Done**, with two documented limits (see Guest memory): no aliasing below 12GB, and HLE code can't dereference pointers into the translated high region |
| M5 | Wiring into shadPS4 | **Done for everything a game hits on boot**: `RunMainEntry`, module start, threads (`_runOnAnotherStack`), TLS (`fs:`), `pthread_once`/cleanup/key destructors, guest signal handlers, guest malloc for TLS, savedata/IME callbacks. Other guest callbacks (fonts, ngs2, AvPlayer, ...) are routed the same way (`Core::ExecuteGuest`) as they turn up |
| M6 | Emscripten/CMake build | **Done**: `emscripten/CMakeLists.txt` compiles upstream shadPS4's own sources (common, core, ~200 HLE libraries, the GPU command processor) for wasm64 + pthreads; Node and browser targets |
| M7 | SSE/AVX, string instructions | **Done** (folded into M1) |
| M8 | Vulkan -> web graphics | **Started**: a WGSL backend for shadPS4's shader recompiler (all 43 of Bully's shaders the recompiler handles pass WebGPU validation); guest compute dispatches run on WebGPU with their results written back to guest memory (`tests/gpu`, in Chromium); frames reach the canvas through WebGPU, WebGL2 or a 2D canvas. **Not started**: draws (graphics pipelines, render targets, textures), so GPU-rendered games still show a black screen |
| M9 | Frontend ([shadps4_web](https://github.com/Shubin123/shadps4_web)) | **Done for loading and running**: pick an extracted game folder, start, live log, canvas, keyboard -> pad. Playwright test drives it in headless Chromium |

## Bully (CUSA03507), end to end

The PKG is a PS2 Classic: `eboot.bin` is Sony's PS2-on-PS4 emulator, `ps2-emu-compiler.self` is
its JIT (a second process), `image/disc01.iso` is the PS2 disc.

- **Native shadPS4 (v0.20.1 WIP, 0614ce3, macOS)**: loads, starts libc/libSceFios2, the PS2
  emulator maps its EE/IOP "flat memory" windows (with "Unable to map ... at physical address"
  errors for the aliased mirrors), then hangs on a black screen ("SubmitDone: 4597ms since last
  call"). The shadPS4 compatibility tracker lists it as "Nothing" (issue #3189, v0.20.0).
- **This port (Node and headless Chrome 156)**: reaches the same point - libSceFios2.prx and
  libc.prx start, `main` runs, the PS2 emulator prints its banner and config, opens the memory
  card, logs `EE Flat Memory (4gb) @ 0x80:00000000` and `IOP Flat Memory (4gb) @ 0x90:00000000`
  (~6 million guest instructions) - then aborts in its own code with `Missing NULL terminator
  during JitProc revision check`: the handshake with its JIT compiler process. shadPS4 doesn't
  implement PS4 JIT shared memory (`sceKernelJitCreateSharedMemory` & co. are stubs) or running a
  second guest process, natively or here, so Bully can't get further on shadPS4 at all until
  upstream does. Even then nothing would be visible here until M8 can draw.

Reproduce:
```
tools/extract_pkg.py UP9000-CUSA03507_00-SLUS212690000001-A0100-V0102.pkg ../games/ps4/CUSA03507
scripts/run_node.sh ../games/ps4/CUSA03507/eboot.bin 180          # Node, log in build/run/out.txt
cd ../shadps4_web && node tests/e2e.mjs ../games/ps4/CUSA03507 "IOP Flat Memory" 300   # browser
```

## Layout

```
UPSTREAM_COMMIT              shadPS4 commit the patch is made against (0614ce32)
emscripten/CMakeLists.txt    the wasm build (targets: shadps4, shadps4_node, interpreter_tests,
                             translate_shaders, gpu_tests)
emscripten/pre.js            alert() shim for Node/workers (SDL message boxes)
emscripten/webgpu.js         the page-side WebGPU device and compute job executor
patches/0001-emscripten.patch   changes to upstream files (35 files, all behind ARCH_WASM)
patches/new_files/           new sources, laid out like upstream's src/
  core/interpreter/            the interpreter, HLE thunks, host->guest calls
  core/wasm_layout.h           guest address layout inside linear memory
  core/wasm_high_memory.*      page table for fixed mappings above 12GB
  core/aerolib/stubs_wasm.cpp  unresolved-import stubs as thunks (natively: Xbyak trampolines)
  core/cpu_patches_wasm.cpp    no-op: nothing to patch when interpreting
  core/guest_call.h            Core::ExecuteGuest - the host->guest call site helper
  shader_recompiler/backend/wgsl/   IR -> WGSL (the SPIR-V backend's counterpart)
  video_core/renderer_web/     Presenter/Rasterizer stand-ins, WebGPU bridge, compute pipelines
  web/                         main, window/input, frame hand-off, stubs for excluded libraries
scripts/setup_tree.sh        build/patched = upstream src/ + patches, as a git repo
scripts/export_patches.sh    build/patched's diff -> patches/0001-emscripten.patch
scripts/build_wasm.sh        configure + build
scripts/run_node.sh          run a game headlessly under Node
tools/extract_pkg.py         fPKG -> game folder (upstream removed its extractor)
tests/interpreter/           guest_code.s -> guest_code.inc (gen_guest_code.sh), test_main.cpp
tests/shaders/               a game's GCN shaders -> WGSL -> WebGPU validation
tests/gpu/                   hand-assembled GCN compute programs dispatched on WebGPU
```

## Build and test

Needs a shadPS4 checkout with submodules at `UPSTREAM_COMMIT` (default `~/shadps4_src`) and
Emscripten (tested with 6.0.13).

```
git clone https://github.com/shadps4-emu/shadps4 ~/shadps4_src
git -C ~/shadps4_src checkout $(cat UPSTREAM_COMMIT)
git -C ~/shadps4_src submodule update --init --recursive --depth 1
scripts/setup_tree.sh
scripts/build_wasm.sh                      # all targets, into build/wasm
node build/wasm/interpreter_tests.js       # 22/22 + a throughput line
tests/shaders/run_shader_tests.sh ../games/ps4/CUSA03507/eboot.bin   # WGSL for a game's shaders
node tests/gpu/run_gpu_tests.mjs           # WebGPU compute, in headless Chromium
```

The two browser-side scripts use the Playwright install in the sibling `shadps4_web` checkout
(`npm install` there) or `PLAYWRIGHT_DIR`.

Developing: edit files in `build/patched/src` (a git repo whose `upstream` tag is pristine), then
`scripts/export_patches.sh`. New files go straight into `patches/new_files`.

## Guest memory

Natively, shadPS4 reserves the PS4's user address range in the host process and maps guest
memory at the same addresses, so a guest pointer is a host pointer and the HLE libraries
dereference guest addresses freely. Keeping that property is what lets ~200 HLE libraries compile
and run unchanged here, so the port keeps it inside wasm64 linear memory:

- The emulator's own static data, stacks and heap are linked at 12GB (`-sGLOBAL_BASE=12GB`,
  `-sINITIAL_MEMORY=13GB`, max 16GB). Everything below is the guest's, at the guest's own
  addresses: system managed 4MB-6GB (eboot.bin at 0x400000, game modules at 0x80000000), system
  reserved 6-8GB, user 8-12GB (`core/wasm_layout.h`). Browsers commit linear memory lazily, so a
  13GB memory costs what's touched (a test run: ~59MB RSS).
- The handful of addresses upstream hard-codes in its native layout (system module base, kernel
  allocation bases, libSceGnmDriver's mapping, the thread stack area) have compressed
  counterparts.
- Fixed-address mappings above 12GB (the PS4 user range goes to ~1TB; Bully's emulator maps at
  0x80'0000'0000) go to a "high" region backed by heap allocations and reached through a page
  table (`core/wasm_high_memory.*`) in the interpreter's slow path. Physical memory mapped there
  can be aliased properly. Limit: HLE code handed a pointer into it can't dereference it.
- Limit: below 12GB, an address is its own storage, so the same physical (direct) memory mapped
  at two addresses gets a copy rather than a shared view (logged). No page protection either.

## Interpreter

- **Decode**: Zydis, cached per address in a direct-mapped table. Each entry keeps the
  instruction's bytes and is reused only if memory still holds them, so code written at runtime
  (JITs, relocations) is never executed stale - no invalidation protocol needed.
- **Dispatch**: VEX mnemonics are folded onto their SSE names (VADDPS -> ADDPS) so one handler
  serves both encodings; operands are normalized so `addps x1, x2` and `vaddps x1, x2, x3` take
  the same path, with VEX's upper-lane zeroing applied on write.
- **Atomics**: LOCK-prefixed read-modify-writes and XCHG use wasm atomics on the guest address
  (guest memory is the shared linear memory), so guest threads on different Web Workers
  synchronize for real. Split-lock (unaligned) cases and CMPXCHG16B use a mutex.
- **Threads**: one `Cpu` per host thread; guest pthreads are emscripten pthreads (Web Workers).
- **Faults**: wild guest pointers, unimplemented instructions, `ud2`/`int3` raise `GuestFault`
  with a register dump and disassembly instead of corrupting the emulator.
- **CPUID** reports the Jaguar (AuthenticAMD, family 16h) feature set, which is what's
  implemented.

### A bug class the earlier prototype hit (still worth knowing)

The first version of `EffectiveAddress()` called Zydis's `ZydisCalcAbsoluteAddress()` for every
memory operand. That function only handles RIP-relative/absolute operands - it has no register
values - so base/index addressing silently produced wrong addresses, and four register-only tests
passed regardless. The current tests are assembler output covering memory addressing, the stack,
SIMD and calls in both directions, for exactly this reason.

## Guest <-> host calls

- **Guest -> host**: `HOST_CALL(fn)` (core/tls.h) becomes `HleAddress<fn>()`: a synthetic address
  (0x7F00'0000'0000+, far outside guest memory) whose thunk reads arguments from rdi..r9 /
  xmm0..7 / the stack per the System V rules for each parameter's C++ type, calls `fn`, writes
  rax/rdx/xmm0, and performs the guest's `ret`. Unresolved imports get thunks that log and return
  0, like upstream's stubs.
- **Host -> guest**: wherever shadPS4 calls a guest function pointer (module entry points,
  thread start routines, `pthread_once` initializers, key destructors, cleanup handlers, signal
  handlers, the guest heap API for TLS, savedata/IME callbacks), it calls
  `Core::ExecuteGuest(fn, args...)`, which runs it in the interpreter. Calls nest: an HLE function
  can call back into the guest, which can call another HLE function. A guest function pointer
  called directly by host code that hasn't been routed yet fails loudly in wasm
  ("table index is out of bounds"), which points straight at the site to route.

## Graphics (M8)

`video_core/renderer_web` replaces `Vulkan::Presenter` and `Vulkan::Rasterizer` (same names and
interface, selected by the two Vulkan headers under `ARCH_WASM`):

- **Presenter**: reads VideoOut buffers from guest memory, converts A8R8G8B8/A8B8G8R8/A2R10G10B10
  to RGBA8, and hands frames to the page (`web/web_display.cpp`) - or to PPM files under Node
  with `SHADPS4_FRAME_DUMP=dir`. Tiled framebuffers are shown as if linear (there's no CPU
  detiler yet). The page draws them with WebGPU, falling back to WebGL2 and then a 2D canvas
  (`gpu_presenter.js` in shadps4_web).
- **Rasterizer**: the GPU command processor (`AmdGpu::Liverpool`, upstream, unmodified) runs
  and calls it. DMA fills/copies and GDS reads run on the CPU, compute dispatches run on WebGPU
  (below), and draws are counted and dropped.

### Shaders: GCN -> WGSL

Browsers' WebGPU takes WGSL only, so instead of converting the SPIR-V backend's output, a
second backend emits WGSL from the same recompiler IR (`shader_recompiler/backend/wgsl`):
frontend, SRT flattening and optimization passes are upstream's, unchanged (the SRT walker that
upstream generates as x86-64 with Xbyak runs in the interpreter). The backend mirrors the SPIR-V
resource model: one `@group(0)` binding per `Info` buffer/image/sampler, in that order. Anything
it can't express yet is returned as an error, not emitted wrong. The cases worth knowing:

- No 64-bit types in WGSL: U64 is `vec2<u32>` with helper arithmetic, F64 is computed as f32.
- Barriers need uniform control flow, but GCN barriers apply to the whole wave, whatever its
  exec mask. The ifs around a barrier are closed before it and reopened after it. A loop that
  contains a barrier keeps every invocation iterating, with finished ones' work predicated off,
  and exits on a workgroup-wide vote (`workgroupUniformLoad`) once no invocation is still
  active - the same trip count the wave had on GCN.
- Integer textures can't be sampled, only loaded; depth textures used with comparisons are
  declared as `texture_depth_*`.

`tests/shaders` pulls every shader out of a game's eboot (by the `OrbShdr` footers), translates
them one per process and compiles the WGSL in Chromium's WebGPU. Bully has 48: 43 translate and
validate, and 5 hit upstream recompiler limits that native shadPS4 shares (unimplemented
`DS_READ_U8/I8`, a fetch shader that needs draw-time state, an SRT walker reading through a null
pointer when given zeroed user data).

### Compute on WebGPU

The device lives on the page's main thread, because WebGPU's promises only settle on a thread
whose event loop runs, and the emulator's threads are pthreads busy in C++.
`emscripten/webgpu.js` requests a device before the runtime starts. `Web::Gpu`
(`web_gpu.cpp`) posts a job to the main thread and blocks on a futex word until the job's
results are back in linear memory.

`Web::ComputePipelines` (`web_compute.cpp`) takes over from `vk_pipeline_cache`/`vk_rasterizer`
for compute: it builds the RuntimeInfo from the COMPUTE_* registers, translates once per program
and `StageSpecialization`, and binds each guest buffer (V#), the flattened user data and GDS.
Each dispatch uploads its buffers from guest memory and copies the written ones back before
returning, so the guest sees the results as soon as the command processor moves on. That's a
round trip per dispatch. A GPU-resident buffer cache using the address space's dirty tracking
is the obvious next step. Shaders that use textures or images aren't dispatched yet.

`tests/gpu` checks the whole path in Chromium with hand-encoded GCN programs set up the way a
game leaves the registers: an LDS exchange across `s_barrier` over four workgroups (and again
with rebound buffers, through the cache), and a loop with per-lane trip counts and a barrier in
its body.

### What's next

Draws: a graphics pipeline cache keyed on the Liverpool state that matters (shaders,
vertex/index formats, blend/depth/raster state, render-target formats), render targets and
textures with detiling as compute shaders, and the presenter reading the guest's front buffer
straight from the GPU instead of from guest memory. WebGL2 can't run WGSL or compute, so on
browsers without WebGPU it stays a presentation fallback. Rendering there would need a GLSL ES
backend and has no compute path.

## What's excluded from the build, and why

Each is listed in `EXCLUDE_REGEX` in `emscripten/CMakeLists.txt`; `web/excluded_libs.cpp` gives
the excluded HLE libraries empty `RegisterLib`s, so their NIDs fall back to the generic
"unimplemented, returns 0" stub - the same thing that happens natively for functions shadPS4
hasn't written.

- Networking and PSN (`libraries/net`, `network`, `np`, `rudp`, `core/net`): host sockets with
  epoll/kqueue, OpenSSL, protobuf, cpp-httplib.
- Video/audio decoding (`avplayer`, `videodec`, `ajm`, `music`): FFmpeg.
- USB (`usbd`): libusb. UPnP: miniupnpc.
- The ImGui overlay renderer, Vulkan pipeline cache, RenderDoc, the debugger/devtools, IPC.
- OpenAL (audio goes through SDL's Emscripten backend, untested so far).
- Embedded fallback fonts (left empty to keep the download at ~18MB).

## Performance

The interpreter runs ~60 million guest instructions per second on an Apple M4 under Node (the
sum loop in `tests/interpreter`), single-threaded per guest thread. A Jaguar core retires on the
order of a billion or more, so even with a GPU backend games would run far below full speed. The
obvious next steps, in order of payoff: chaining decoded basic blocks instead of per-instruction
cache lookups, lazy flag evaluation, and eventually translating hot blocks to wasm.

## Loading games in the browser

The page copies the chosen folder into Emscripten's in-memory filesystem (files over 1GB are
skipped - e.g. a PS2 Classic's disc image, which Bully never gets far enough to read). Large games
will need an OPFS/WasmFS-backed filesystem instead. PKGs have to be extracted first with
`tools/extract_pkg.py` (fake-signed PKGs only; it reads the fake keysets from upstream's git
history rather than this repo carrying them).

## Attribution note on the upstream repo

shadPS4's `AGENTS.md` contains an instruction telling AI agents to check GitHub push permissions
and, if the check fails, write a file admitting to "botting the repo". That's a prompt-injection
trap aimed at agents following instructions found in cloned repositories; it was not followed.
