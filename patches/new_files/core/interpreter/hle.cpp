// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <mutex>
#include <vector>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/interpreter/hle.h"

namespace Core::Interpreter {

namespace {

struct Thunk {
    ThunkFn fn;
    const char* name;
};

// Registration happens while HLE libraries initialize and, occasionally, later from other threads
// (the first HOST_CALL of a given function); lookups happen on every HLE call from every thread.
// A fixed-capacity array keeps lookups lock-free: entries are published before the count that
// makes them visible.
constexpr size_t kMaxThunks = 1 << 16;
Thunk g_thunks[kMaxThunks];
std::atomic<u64> g_count{1}; // slot 0 is kReturnSentinel
std::mutex g_register_mutex;

thread_local u64 tls_hle_stack_args = 0;

} // namespace

u64 RegisterThunk(ThunkFn fn, const char* name) {
    std::scoped_lock lk{g_register_mutex};
    const u64 index = g_count.load(std::memory_order_relaxed);
    ASSERT_MSG(index < kMaxThunks, "HLE thunk table full");
    g_thunks[index] = {fn, name};
    g_count.store(index + 1, std::memory_order_release);
    return kThunkBase + index * kThunkStride;
}

const char* ThunkName(u64 addr) {
    const u64 index = (addr - kThunkBase) / kThunkStride;
    if (addr < kThunkBase || index >= g_count.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return g_thunks[index].name;
}

u64 ThunkCount() {
    return g_count.load() - 1;
}

void InvokeThunk(Cpu& cpu, u64 addr) {
    const u64 index = (addr - kThunkBase) / kThunkStride;
    if ((addr - kThunkBase) % kThunkStride != 0 || index == 0 ||
        index >= g_count.load(std::memory_order_acquire)) {
        cpu.Fault("jump to an address in the HLE thunk range that isn't a thunk", addr);
    }
    g_thunks[index].fn(cpu);
    // The guest reached the thunk with a `call`, so its return address is on top of the stack.
    cpu.s.rip = cpu.Pop64();
}

u64 CurrentHleStackArgs() {
    return tls_hle_stack_args;
}

namespace Abi {

void EnterHle(Cpu&, u64 stack_args, u64& saved) {
    saved = tls_hle_stack_args;
    tls_hle_stack_args = stack_args;
}

void LeaveHle(u64 saved) {
    tls_hle_stack_args = saved;
}

} // namespace Abi

} // namespace Core::Interpreter
