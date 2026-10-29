// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The host -> guest direction: how shadPS4's C++ code runs guest functions in the wasm build.
// Natively it just calls them (they're x86 code in the same process); here each such call site
// goes through Core::ExecuteGuest (core/guest_call.h), which lands in CallGuest below.

#pragma once

#include <type_traits>
#include "common/types.h"
#include "core/interpreter/cpu.h"

namespace Core::Interpreter {

// Arguments for one guest call, assigned per the System V ABI as they're added.
struct GuestArgs {
    u64 gp[6]{};
    int ngp{};
    Vec vec[8]{};
    int nvec{};

    template <typename T>
    void Add(T v) {
        using U = std::remove_cvref_t<T>;
        if constexpr (std::is_same_v<U, float>) {
            vec[nvec] = {};
            vec[nvec++].f[0] = v;
        } else if constexpr (std::is_same_v<U, double>) {
            vec[nvec] = {};
            vec[nvec++].fd[0] = v;
        } else if constexpr (std::is_pointer_v<U> || std::is_null_pointer_v<U>) {
            gp[ngp++] = reinterpret_cast<u64>(v);
        } else {
            static_assert(std::is_integral_v<U> || std::is_enum_v<U>,
                          "only integer, pointer and float arguments are passed to guest code");
            gp[ngp++] = static_cast<u64>(v);
        }
    }
};

struct GuestResult {
    u64 rax;
    u64 rdx;
    Vec xmm0;
};

// Calls guest code at `addr` on the current thread's guest context and returns when it does. If
// the thread is already inside guest code (an HLE function calling back into the guest), the
// call nests below the guest's current stack frame; otherwise it runs on a per-thread guest
// stack allocated on first use.
GuestResult CallGuestRaw(u64 addr, const GuestArgs& args);

template <typename R = u64, typename... A>
R CallGuest(u64 addr, A... a) {
    GuestArgs args;
    (args.Add(a), ...);
    const GuestResult r = CallGuestRaw(addr, args);
    if constexpr (std::is_void_v<R>) {
        return;
    } else if constexpr (std::is_same_v<R, float>) {
        return r.xmm0.f[0];
    } else if constexpr (std::is_same_v<R, double>) {
        return r.xmm0.fd[0];
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>(r.rax);
    } else {
        return static_cast<R>(r.rax);
    }
}

// Starts a guest thread's root function: func(arg) on the guest stack whose top is `stack_top`.
// Returns its rax. Also accepts a host function pointer (shadPS4 sometimes runs its own helpers
// on a guest-allocated stack), which is simply called.
u64 RunOnGuestStack(u64 func, u64 arg, u64 stack_top);

// The process entry: replicates Linker::RunMainEntry's stack setup (EntryParams copied to the top
// of a fresh stack, rdi = params, rsi = exit function) and runs the eboot's entry point.
[[noreturn]] void RunMainEntry(u64 entry, u64 params, u64 exit_func);

// Whether a function pointer value names host code (a wasm table index) rather than guest code.
inline bool IsHostFunction(u64 ptr) {
    return ptr < 0x400000;
}

// Logs a register dump + disassembly around the current thread's rip, for fault reports.
void DumpCurrentState(const char* why);

} // namespace Core::Interpreter
