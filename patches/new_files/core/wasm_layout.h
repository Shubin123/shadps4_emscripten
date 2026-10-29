// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Guest virtual address layout for the wasm build.
//
// Natively, shadPS4 reserves the PS4's user address range (0x400000 up to ~0x5FFF'FFFF'FFFF) in
// the host process and maps guest memory at the same addresses, so a guest pointer *is* a host
// pointer and all the HLE code can dereference guest addresses directly. The wasm build keeps
// that property - it's what lets core/libraries/** compile and run unmodified - by placing guest
// memory at the same numbers inside wasm64 linear memory. Linear memory is capped at 16GB, so:
//
//   [0,        4MB)   unused (guest null-pointer page)
//   [4MB,      6GB)   system managed  (eboot.bin at 0x400000, game modules at 0x80000000)
//   [6GB,      8GB)   system reserved (system modules, kernel-internal allocations)
//   [8GB,     12GB)   user
//   [12GB,    16GB)   the emulator itself: wasm static data, C stacks, malloc heap
//
// The last line is arranged at link time with -sGLOBAL_BASE=12GB (see emscripten/CMakeLists.txt).
// Pages are committed lazily by the browser, so this costs nothing until touched. A guest that
// insists on a fixed address outside these ranges (some do, e.g. Sony's PS2 emulator maps
// 0x80'0000'0000) cannot be satisfied and gets an error, exactly as an out-of-range fixed
// mapping does natively.

#pragma once

#include "common/types.h"

namespace Core::WasmLayout {

constexpr VAddr SystemManagedMin = 0x400000ULL;
constexpr VAddr SystemManagedMax = 0x17FFFFFFFULL;
constexpr VAddr SystemReservedMin = 0x180000000ULL;
constexpr VAddr SystemReservedMax = 0x1FFFFFFFFULL;
constexpr VAddr UserMin = 0x200000000ULL;
constexpr VAddr UserMax = 0x2FFFFFFFFULL;

// Where emscripten's own data/stack/heap begin (must match -sGLOBAL_BASE).
constexpr VAddr HostBase = 0x300000000ULL;

// Native shadPS4 hard-codes a few addresses inside the system reserved range (0x7FFFFC000+);
// these are the same offsets' counterparts inside the compressed range above.
constexpr VAddr SystemModuleLoadBase = SystemReservedMin;
constexpr VAddr KernelFlexibleBase = SystemReservedMin + 0x40000000ULL; // native 0x880000000
constexpr VAddr KernelTlsAllocBase = KernelFlexibleBase + 0x1000000ULL; // native 0x881000000
// libSceGnmDriver's fixed direct-memory mapping, natively 0xfe0000000 near the top of system
// reserved.
constexpr VAddr GnmDriverBase = SystemReservedMax + 1 - 0x20000000ULL;
// Top of the guest thread-stack area (natively 0x7EFFF8000, 256MB below the end of system
// managed); stacks are carved downward from here.
constexpr VAddr UsrStack = SystemManagedMax + 1 - 0x10004000ULL;

} // namespace Core::WasmLayout
