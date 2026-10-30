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
| M3 | Tests | **Done**: 27 checks against assembler-generated x86-64 (`tests/interpreter`), run under Node |
| M4 | Guest virtual memory | **Done**, with two documented limits (see Guest memory): no aliasing below 12GB, and HLE code can't dereference pointers into the translated high region |
| M5 | Wiring into shadPS4 | **Done for everything a game hits on boot**: `RunMainEntry`, module start, threads (`_runOnAnotherStack`), TLS (`fs:`), `pthread_once`/cleanup/key destructors, guest signal handlers, guest malloc for TLS, savedata/IME callbacks. Other guest callbacks (fonts, ngs2, AvPlayer, ...) are routed the same way (`Core::ExecuteGuest`) as they turn up |
| M6 | Emscripten/CMake build | **Done**: `emscripten/CMakeLists.txt` compiles upstream shadPS4's own sources (common, core, ~200 HLE libraries, the GPU command processor) for wasm64 + pthreads; Node and browser targets |
| M7 | SSE/AVX, string instructions | **Done** (folded into M1) |
| M8 | Vulkan -> web graphics | **Draws and compute run on WebGPU**: GCN shaders recompiled to WGSL; graphics pipelines from the Liverpool state, render targets, depth/stencil, blending, textures (detiled and converted on the CPU), samplers, fast clears, every primitive type (the ones WebGPU lacks emulated), indirect draws; compute with buffers, textures and written images; display buffers read back from the GPU at flip. Checked pixel by pixel in `tests/gpu` (Chromium and Node/Dawn), and Bloodborne renders its boot logo through it (see below). **Not yet**: geometry/tessellation stages (WebGPU has neither), MSAA, writing GPU results back to guest memory for the CPU |
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
  upstream does.

Reproduce:
```
tools/extract_pkg.py UP9000-CUSA03507_00-SLUS212690000001-A0100-V0102.pkg ../games/ps4/CUSA03507
scripts/run_node.sh ../games/ps4/CUSA03507/eboot.bin 180          # Node, log in build/run/out.txt
cd ../shadps4_web && node tests/e2e.mjs ../games/ps4/CUSA03507 "IOP Flat Memory" 300   # browser
```

## Bloodborne (CUSA03173), end to end

The EU release. Its 30GB of files are too much for the browser's in-memory filesystem, so it
runs in the Node build on Dawn's WebGPU (`scripts/run_node.sh`), reading the game from disk.
The results below are from version 01.00; with the 1.09 update next to it (as shadPS4 expects
updates: extracted to a sibling `<game folder>-UPDATE`, whose files take precedence) the loader
reports `App Version: 01.09`, loads the update's executable, and Paleblood installs all 32 of its
replacements; the game then boots and renders the same way with them running. Upstream's macOS
build (same revision) recognizes the same folder.

- It boots: modules load, `main` runs, threads start, its network setup gets "no network"
  answers from the offline libSceNet instead of a NULL errno pointer (which crashed it before),
  and it streams its data (`dvdroot_ps4/...`) through the file system.
- Its GPU work runs on WebGPU: compute passes writing lookup tables and images, depth passes,
  textured draws and a colour-grading pass over the display buffers. In the first 512 submits,
  2554 of 2554 draws and 3659 of 3659 dispatches ran, and every flip was read back from the GPU.
- What it shows: first mid-gray (the grading pass maps a 1920x1080 buffer the game fills with
  `0xFF808080` before any GPU work, traced with `SHADPS4_WEBGPU_TRACE=1`), then black, then its
  boot logo - "JAPAN Studio", a BC7 texture drawn over black and graded - rendered correctly,
  about 5.5 minutes and 500 frames after the first one (Apple M4, Dawn on Metal).

```
tools/extract_pkg.py CUSA03173.pkg ../games/ps4/CUSA03173
tools/extract_pkg.py CUSA03173-update-1.09.pkg ../games/ps4/CUSA03173-UPDATE   # optional
scripts/run_node.sh ../games/ps4/CUSA03173/eboot.bin 1800       # frames in build/run/frames
```

`SHADPS4_WEBGPU_TRACE=1` logs every draw (shaders, targets, bindings) and texture upload;
`SHADPS4_WGSL_DUMP=dir` writes every translated shader.

## Layout

```
UPSTREAM_COMMIT              shadPS4 commit the patch is made against (0614ce32)
emscripten/CMakeLists.txt    the wasm build (targets: shadps4, shadps4_node, interpreter_tests,
                             translate_shaders, gpu_tests)
emscripten/pre.js            alert() shim for Node/workers (SDL message boxes); a page's
                             environment variables (Module.shadps4Env) for getenv
emscripten/webgpu.js         the page-side WebGPU device and the batch executor (draws, dispatches, ...)
patches/0001-emscripten.patch   changes to upstream files (36 files, all behind ARCH_WASM)
patches/new_files/           new sources, laid out like upstream's src/
  core/interpreter/            the interpreter, HLE thunks, host->guest calls
  core/wasm_layout.h           guest address layout inside linear memory
  core/wasm_high_memory.*      page table for fixed mappings above 12GB
  core/aerolib/stubs_wasm.cpp  unresolved-import stubs as thunks (natively: Xbyak trampolines)
  core/cpu_patches_wasm.cpp    no-op: nothing to patch when interpreting
  core/guest_call.h            Core::ExecuteGuest - the host->guest call site helper
  core/paleblood/              Paleblood's game function replacements: hook installer, and the
                               runtime interface they're written against, on the interpreter
  shader_recompiler/backend/wgsl/   IR -> WGSL (the SPIR-V backend's counterpart)
  video_core/renderer_web/     Presenter/Rasterizer stand-ins and the WebGPU renderer: batches
                               (web_gpu), shader cache, draws (web_graphics), compute, resource
                               binding, surfaces (textures/render targets), CPU detiler
  web/                         main, window/input, frame hand-off, offline libSceNet/NetCtl,
                               stubs for excluded libraries
scripts/setup_tree.sh        build/patched = upstream src/ + patches, as a git repo
scripts/export_patches.sh    build/patched's diff -> patches/0001-emscripten.patch
scripts/build_wasm.sh        configure + build
scripts/run_node.sh          run a game headlessly under Node (on Dawn's WebGPU when installed)
scripts/node_webgpu.mjs      gives Node a WebGPU implementation (`webgpu` package, Dawn)
scripts/sync_paleblood.sh    Paleblood checkout -> third_party/paleblood
third_party/paleblood/       Paleblood (Bloodborne decompilation), the parts the build compiles
tools/extract_pkg.py         fPKG -> game folder (upstream removed its extractor)
tools/paleblood_port.py      third_party/paleblood -> build tree, ported for interpretation
tests/interpreter/           guest_code.s -> guest_code.inc (gen_guest_code.sh), test_main.cpp
tests/shaders/               a game's GCN shaders -> WGSL -> WebGPU validation
tests/gpu/                   hand-assembled GCN dispatches and draws on WebGPU, checked pixel
                             by pixel
```

## Build and test

Needs a shadPS4 checkout with submodules at `UPSTREAM_COMMIT` (default `../upstream_native/shadps4`)
and Emscripten (tested with 6.0.13).

```
git clone https://github.com/shadps4-emu/shadps4 ../upstream_native/shadps4
git -C ../upstream_native/shadps4 checkout $(cat UPSTREAM_COMMIT)
git -C ../upstream_native/shadps4 submodule update --init --recursive --depth 1
scripts/setup_tree.sh
scripts/build_wasm.sh                      # all targets, into build/wasm
node build/wasm/interpreter_tests.js       # 27/27 + a throughput line
tests/shaders/run_shader_tests.sh ../games/ps4/CUSA03507/eboot.bin   # WGSL for a game's shaders
node tests/gpu/run_gpu_tests.mjs           # WebGPU dispatches and draws, in headless Chromium
(cd build/wasm && SHADPS4_GPU_TEST_ENV=page node --import ../../scripts/node_webgpu.mjs gpu_tests_node.js)
                                           # the same under Node, on Dawn
```

The browser-side scripts use the Playwright install in the sibling `shadps4_web` checkout
(`npm install` there) or `PLAYWRIGHT_DIR`; the Node GPU runs use the `webgpu` package installed
there too, or `NODE_WEBGPU_DIR`.

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
  can call back into the guest, which can call another HLE function. Arguments past the six
  integer and eight vector registers go on the guest stack, as the ABI places them. A guest
  function pointer called directly by host code that hasn't been routed yet fails loudly in wasm
  ("table index is out of bounds"), which points straight at the site to route.

## Paleblood: compiled game functions (Bloodborne)

[Paleblood](https://github.com/Yharnam-Hunters/Paleblood) is a functional decompilation of
Bloodborne (CUSA03173, version 1.09): game functions rewritten as readable C++, each one proven
upstream to behave exactly like the original machine code. Paleblood runs them natively, on its
own Linux runtime, by writing a jump at each original's entry. It is GPL-2.0-or-later like
shadPS4, and the parts that compile here are vendored in `third_party/paleblood`, unchanged.

Paleblood's own runtime doesn't carry over. It is a native x86-64 Linux loader and system
library layer, a different solution to what shadPS4's linker and HLE libraries already do here.
What does carry over is the replacements: for an interpreter, every one is a game function that
runs as compiled wasm instead of being interpreted one x86 instruction at a time. The
`BB_LIMITER_WAIT=sleep` option also stops the frame limiter spinning a guest thread (and the
interpreter with it) through the rest of each frame.

- **Installing**: once the executable is loaded and relocated, and only for CUSA03173 version
  01.09 (other versions put the code elsewhere), `core/paleblood` writes the same 14-byte
  `jmp [rip+0]` Paleblood writes natively at each original's entry. Here it jumps to the
  replacement's HLE thunk, which marshals the arguments from the guest registers and returns
  to the original caller. Functions shorter than the jump are left alone, and `BB_HOOK_SKIP`
  works as upstream. `SHADPS4_PALEBLOOD=0` turns the replacements off.
- **Their view of the game**: the replacements reach the rest of the game by name through
  Paleblood's runtime interface (`RT_ORIGINAL`, `RT_GLOBAL`). The port implements it on the
  interpreter (`core/paleblood/port/runtime`): a call to an original runs in the interpreter,
  nested on the calling guest thread, and a global is plain memory, since guest addresses are
  linear-memory addresses here.
- **Build-time changes**: `tools/paleblood_port.py` makes two kinds of change, in a copy in the
  build tree. Vtable slots become `rt::guest_fn` (the same 8 bytes, called through the
  interpreter, because the target is x86 and not a wasm function). The thread-pointer read
  (`movq %gs:0`) becomes a read of the guest thread's `fs:[0]`. Anything in a form the script
  doesn't recognize stops the build.
- **Tested**: `tests/interpreter` hooks a guest function with a replacement written the
  Paleblood way. It checks that callers reach the replacement and that the replacement can call
  an original, read a global, call through a vtable slot in guest memory and read the thread
  pointer. It also checks that a function too short for the jump is left alone. With Bloodborne
  1.09 (the base game plus its update), all 32 install and the game boots and renders with them
  in place (see "Bloodborne, end to end").

## Graphics (M8)

`video_core/renderer_web` replaces `Vulkan::Presenter` and `Vulkan::Rasterizer` (same names and
interface, selected by the two Vulkan headers under `ARCH_WASM`):

- **Rasterizer**: the GPU command processor (`AmdGpu::Liverpool`, upstream, unmodified) runs
  and calls it. Draws and compute dispatches run on WebGPU, recorded into a batch that goes to
  the page at the end of each submit (below). DMA fills/copies and GDS reads run on the CPU and
  tell the renderer which guest memory changed.
- **Presenter**: at each flip, the VideoOut buffer is read back from the GPU if the guest
  rendered it there (or a shader wrote it), otherwise read from guest memory, detiled on the
  CPU if it's tiled. Either way it's converted to RGBA8 and handed to the page
  (`web/web_display.cpp`), or to PPM files under Node with `SHADPS4_FRAME_DUMP=dir`. The page
  draws frames with WebGPU, falling back to WebGL2 and then a 2D canvas (`gpu_presenter.js` in
  shadps4_web).

### Shaders: GCN -> WGSL

Browsers' WebGPU takes WGSL only, so instead of converting the SPIR-V backend's output, a
second backend emits WGSL from the same recompiler IR (`shader_recompiler/backend/wgsl`):
frontend, SRT flattening and optimization passes are upstream's, unchanged (the SRT walker that
upstream generates as x86-64 with Xbyak runs in the interpreter). The backend mirrors the SPIR-V
resource model: one binding per `Info` buffer/image/sampler, in that order, in `@group(0)` for
compute and vertex shaders and `@group(1)` for pixel shaders, so a draw's two stages share a
pipeline layout. Anything it can't express yet is returned as an error, not emitted wrong. The
cases worth knowing:

- No 64-bit types in WGSL: U64 is `vec2<u32>` with helper arithmetic, F64 is computed as f32.
- Barriers need uniform control flow, but GCN barriers apply to the whole wave, whatever its
  exec mask. The ifs around a barrier are closed before it and reopened after it. A loop that
  contains a barrier keeps every invocation iterating, with finished ones' work predicated off,
  and exits on a workgroup-wide vote (`workgroupUniformLoad`) once no invocation is still
  active - the same trip count the wave had on GCN.
- Vertex shaders fetch their own attributes: the wasm build translates the fetch shader inline
  (upstream's `inline_fetch_shader` option, always on), as buffer loads indexed by the vertex
  id. WebGPU's vertex formats and strides are narrower than the guest's, and rectangle lists
  need vertices fetched by computed index.
- A vertex shader's entry point is made per draw (`LinkVertexShader`): its outputs must match
  the pixel shader's inputs exactly, interpolation included (ones the program doesn't write are
  zero), the guest's viewport transform is applied there (WebGPU viewports must lie inside the
  render target, guest ones needn't), and rectangle lists are expanded there: each rectangle is
  two triangles, its fourth corner computed from the other three as upstream's tessellation
  shader does.
- Sampled images get the T#'s component swizzle applied in the shader (WebGPU texture views
  have none). Written images are storage textures in the WebGPU format matching the guest's
  (`StorageTexelFormat`; several need the `texture-formats-tier1` feature, reading back what's
  written needs tier2). Cube maps a shader writes are 2D arrays of faces, as GCN addresses them.
- Integer textures can't be sampled, only loaded; depth textures used with comparisons are
  declared as `texture_depth_*`; 1D images are 2D textures one texel high.

`tests/shaders` pulls every shader out of a game's eboot (by the `OrbShdr` footers), translates
them one per process and compiles the WGSL in Chromium's WebGPU. Bully has 48: 43 translate and
validate, and 5 hit upstream recompiler limits that native shadPS4 shares (unimplemented
`DS_READ_U8/I8`, a fetch shader that needs draw-time state, an SRT walker reading through a null
pointer when given zeroed user data).

### Batches

The device lives on the page's main thread, because WebGPU's promises only settle on a thread
whose event loop runs, and the emulator's threads are pthreads busy in C++.
`emscripten/webgpu.js` requests a device before the runtime starts (under Node, from Dawn's
`webgpu` package: `scripts/node_webgpu.mjs`). Everything the renderer does is recorded on the
command processor's thread as a batch (`web_gpu.h`): JSON commands (create a texture, upload
into it, draw, dispatch, read back) plus an upload area holding the guest memory they read,
copied when the command is recorded. A batch is posted to the main thread at the end of each
submit and runs there strictly after the previous one, as one command encoder; consecutive draws
into the same targets share a render pass. Nothing waits unless results are needed back in
memory: a dispatch that writes guest buffers, and the presenter's readback. Shader modules and
pipelines are created once and referred to by id after that (`web_shaders.h`).

### Draws

`Web::GraphicsPipelines` (`web_graphics.cpp`) does what `vk_rasterizer`'s `Draw` and
`vk_pipeline_cache`'s graphics half do. It builds the vertex and pixel shaders' RuntimeInfo from
the registers, translates them through the shader cache, and turns the fixed-function state into
a WebGPU render pipeline: topology, culling and winding, depth/stencil test and ops, depth bias,
per-target blending and write masks (swizzled like the export), unclipped depth when the guest
disables Z clipping. What WebGPU lacks is emulated:

- quads, quad strips, fans, polygons and line loops are rewritten as index lists on the CPU;
  rectangle lists are drawn by the vertex shader as above;
- the viewport transform runs in the vertex shader, against a viewport covering the target
  (including the clip-disabled case, where positions arrive in window coordinates);
- fast clears: CMASK/HTILE fills and clear shaders are noted (as upstream's texture cache does)
  and the next draw into the target starts from its clear color or depth; depth/stencil clear
  enables become load-op clears; fast-clear elimination and MSAA resolve are a clear and a copy;
- indirect draws read their arguments from guest memory and write the base vertex/instance into
  the vertex shader's user data, as the CP does.

Not handled: geometry and tessellation stages (WebGPU has neither), MSAA (targets are rendered
single-sampled), dual-source blending (the second output isn't produced), user clip distances,
logic ops, stencil ops without a WebGPU equivalent, more than one viewport.

### Surfaces

`Web::Surfaces` (`web_surfaces.cpp`) holds the textures and render targets on the GPU, by guest
address. Textures are uploaded from guest memory: detiled on the CPU (`web_tiling.cpp`, a C++
port of upstream's GPU detiler `tiling.comp`, with `texture_cache/tile.h`'s sizes unchanged) and,
where WebGPU has no equivalent format, converted (5:6:5 and other packed formats to RGBA8,
16-bit normalized to half floats, 96-bit to 128). BC1-7 are uploaded as they are. Render targets
and written images start from guest memory too, and from then on the GPU's copy is newer: a
texture over the same memory samples it (a copy, if the T# covers less of it, or the draw also
renders into it), and the presenter reads it back.

Nothing is written back to guest memory. Without page protection there's no telling when the CPU
writes, so content is compared instead: each surface remembers a hash of the guest memory it was
last synced with, rechecked once per frame (and whenever a DMA writes over it). A texture whose
memory changed is uploaded again. A render target whose memory changed was written by the CPU
since the GPU last used it - the GPU never touches guest memory - and the CPU's version wins.
The cost is that guest code reading back what the GPU rendered, or a DMA copying it, sees stale
memory. A GPU-resident buffer cache would remove the per-batch buffer copies too.

### Compute

`Web::ComputePipelines` (`web_compute.cpp`) builds the RuntimeInfo from the COMPUTE_* registers
and binds buffers, user data, GDS, textures and written images the same way as draws. A dispatch
that writes guest buffers is waited for and the written ranges copied back, so the guest sees
the results as soon as the command processor moves on; one that writes only images doesn't wait.
Dispatches that clear render target metadata without computing tiled addresses (upstream's
heuristic) are noted as fast clears instead of run.

### Tests

`tests/gpu` runs hand-encoded GCN programs, set up the way a game leaves the registers, through
the real classes and the page's executor: in headless Chromium (`run_gpu_tests.mjs`) and under
Node on Dawn (`gpu_tests_node.js`). Compute: an LDS exchange across `s_barrier` over four
workgroups (and again with rebound buffers), and a loop with per-lane trip counts and a barrier
in its body. Draws, through `Vulkan::Rasterizer` into a render target read back the way the
presenter reads a display buffer and checked pixel by pixel: a vertex shader that fetches its
attributes through a fetch shader called with `s_swappc` (as compiled PS4 vertex shaders do), a
flat triangle, an interpolated quad list (auto-indexed and with a 16-bit index buffer), a
rectangle list in window space over a CMASK fast clear, alpha blending over CPU-written content,
a texture sampled from guest memory, and a compute shader writing an image, submitted as PM4
through `Liverpool::SubmitGfx`.

### What's next

A buffer cache that keeps guest buffers on the GPU across batches, writing render targets back
to guest memory when the CPU reads them, MSAA, and moving detiling and format conversion to
compute shaders as upstream does. WebGL2 can't run WGSL or compute, so on browsers without
WebGPU it stays a presentation fallback.

## What's excluded from the build, and why

Each is listed in `EXCLUDE_REGEX` in `emscripten/CMakeLists.txt`; `web/excluded_libs.cpp` gives
the excluded HLE libraries empty `RegisterLib`s, so their NIDs fall back to the generic
"unimplemented, returns 0" stub - the same thing that happens natively for functions shadPS4
hasn't written.

- Networking and PSN (`libraries/net`, `network`, `np`, `rudp`, `core/net`): host sockets with
  epoll/kqueue, the host's routing tables, OpenSSL, protobuf, cpp-httplib. In their place,
  `web/net_web.cpp` makes libSceNet and libSceNetCtl answer like a console with no network:
  errno, byte order and address text work, epoll objects exist and time out, sockets and DNS
  fail with ENETDOWN, and NetCtl reports "disconnected". Leaving them as generic stubs isn't
  enough, because games set up sockets at boot (Bloodborne dereferences the errno pointer the
  stub returns as NULL).
- Video/audio decoding (`avplayer`, `videodec`, `ajm`, `music`): FFmpeg.
- USB (`usbd`): libusb. UPnP: miniupnpc.
- The ImGui overlay renderer, Vulkan pipeline cache, RenderDoc, the debugger/devtools, IPC.
- OpenAL (audio goes through SDL's Emscripten backend, untested so far).
- Embedded fallback fonts (left empty to keep the download at ~18MB).

## Performance

The interpreter runs ~60 million guest instructions per second on an Apple M4 under Node (the
sum loop in `tests/interpreter`), single-threaded per guest thread. Game functions with a
Paleblood replacement are the exception: they run as compiled wasm. A Jaguar core retires on the
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
