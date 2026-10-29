// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Page table for guest addresses in the wasm build's translated high region
// (core/wasm_layout.h: HighMin..HighMax). Everything below 12GB is identity-mapped and never
// consults this; addresses up here can't be (linear memory ends at 16GB), so each 16KB guest page
// points at host memory instead. Lookups are lock-free (two loads); updates take a lock.

#pragma once

#include "common/types.h"
#include "core/wasm_layout.h"

namespace Core::HighMemory {

constexpr u64 kPageBits = 14;
constexpr u64 kPageSize = 1ULL << kPageBits;

inline bool InRange(VAddr va) {
    return va >= WasmLayout::HighMin && va <= WasmLayout::HighMax;
}

// Points the guest pages [va, va + size) at host memory [host, host + size). Page aligned.
void Map(VAddr va, u64 size, u8* host);
void Unmap(VAddr va, u64 size);

// Host address for a guest address in the high region, or nullptr if unmapped.
u8* Translate(VAddr va);

} // namespace Core::HighMemory
