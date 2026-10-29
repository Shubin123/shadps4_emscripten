// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <mutex>
#include "common/assert.h"
#include "core/wasm_high_memory.h"

namespace Core::HighMemory {

namespace {

// Two levels: 1GB per second-level table (65536 x 16KB pages, 512KB of pointers), created on
// first use. The high region spans < 1TB, so the first level has under a thousand slots.
constexpr u64 kL2Bits = 30 - kPageBits;
constexpr u64 kL2Entries = 1ULL << kL2Bits;
constexpr u64 kL1Entries = ((WasmLayout::HighMax + 1 - WasmLayout::HighMin) >> 30) + 1;

using L2 = std::array<std::atomic<u8*>, kL2Entries>;
std::array<std::atomic<L2*>, kL1Entries> g_l1{};
std::mutex g_mutex;

L2* GetOrCreateL2(u64 l1) {
    L2* t = g_l1[l1].load(std::memory_order_acquire);
    if (!t) {
        t = new L2{};
        g_l1[l1].store(t, std::memory_order_release);
    }
    return t;
}

} // namespace

void Map(VAddr va, u64 size, u8* host) {
    ASSERT_MSG(va % kPageSize == 0 && size % kPageSize == 0,
               "high mapping {:#x}+{:#x} is not page aligned", va, size);
    std::scoped_lock lk{g_mutex};
    for (u64 off = 0; off < size; off += kPageSize) {
        const u64 rel = va + off - WasmLayout::HighMin;
        L2* t = GetOrCreateL2(rel >> 30);
        (*t)[(rel >> kPageBits) & (kL2Entries - 1)].store(host + off, std::memory_order_release);
    }
}

void Unmap(VAddr va, u64 size) {
    std::scoped_lock lk{g_mutex};
    for (u64 off = 0; off < size; off += kPageSize) {
        const u64 rel = va + off - WasmLayout::HighMin;
        if (L2* t = g_l1[rel >> 30].load(std::memory_order_acquire)) {
            (*t)[(rel >> kPageBits) & (kL2Entries - 1)].store(nullptr, std::memory_order_release);
        }
    }
}

u8* Translate(VAddr va) {
    if (!InRange(va)) {
        return nullptr;
    }
    const u64 rel = va - WasmLayout::HighMin;
    const L2* t = g_l1[rel >> 30].load(std::memory_order_acquire);
    if (!t) {
        return nullptr;
    }
    u8* page = (*t)[(rel >> kPageBits) & (kL2Entries - 1)].load(std::memory_order_acquire);
    return page ? page + (va & (kPageSize - 1)) : nullptr;
}

} // namespace Core::HighMemory
