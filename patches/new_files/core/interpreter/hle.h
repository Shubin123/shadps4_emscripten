// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The guest -> host call boundary.
//
// shadPS4's linker resolves a game's imports to the addresses of its C++ HLE functions
// (LIB_FUNCTION -> HOST_CALL(fn) -> LinkSymbolImpl), and natively the game just calls them: guest
// and host share a CPU and a calling convention (PS4_SYSV_ABI). Under interpretation a host
// function has no x86 address. Instead each one gets a synthetic address in a range no guest
// memory can occupy; when the interpreter's rip lands there it reads the arguments out of the
// guest registers/stack the way the System V AMD64 ABI places them, calls the C++ function,
// writes its result back to rax/xmm0 and performs the guest's `ret`.
//
// The argument marshalling is generated from each function's C++ signature by HleThunk<fn>, so the
// ~5500 LIB_FUNCTION registrations need no per-function code. Under ARCH_WASM, HOST_CALL(fn)
// (core/tls.h) expands to HleAddress<fn>().

#pragma once

#include <cstring>
#include <tuple>
#include <type_traits>
#include "common/types.h"
#include "core/interpreter/cpu.h"

namespace Core::Interpreter {

// Thunk addresses live far above wasm64's 16GB of linear memory, so no guest pointer can alias
// one, but below 2^47 so they still look like canonical user-space addresses to guest code.
constexpr u64 kThunkBase = 0x00007F0000000000ULL;
constexpr u64 kThunkStride = 16;
// Return address CallGuest pushes; reaching it ends the innermost RunUntil.
constexpr u64 kReturnSentinel = kThunkBase;

using ThunkFn = void (*)(Cpu&);

u64 RegisterThunk(ThunkFn fn, const char* name);
// Invokes the thunk at `addr` (which the guest has just `call`ed) and pops the return address.
void InvokeThunk(Cpu& cpu, u64 addr);
const char* ThunkName(u64 addr);
u64 ThunkCount();

// Stack-passed arguments of the HLE call currently executing on this thread: rsp+8 at entry.
// VA_CTX (common/va_ctx.h) uses it as the va_list overflow area for variadic HLE functions.
u64 CurrentHleStackArgs();

namespace Abi {

template <typename T>
inline constexpr bool kIsVector =
    !std::is_arithmetic_v<T> && !std::is_pointer_v<T> && !std::is_enum_v<T> &&
    !std::is_class_v<T> && !std::is_member_pointer_v<T> && sizeof(T) == 16;

// Classifies and fetches one argument per the System V AMD64 rules, for the types HLE functions
// actually take: integers/enums/pointers (and small trivially-copyable structs) in rdi..r9, then
// the stack; float/double/vectors in xmm0..7, then the stack.
struct ArgReader {
    Cpu& cpu;
    int gp = 0;
    int vec = 0;
    u64 stack;

    static constexpr u8 kGpOrder[6] = {RDI, RSI, RDX, RCX, R8, R9};

    u64 NextGp() {
        if (gp < 6) {
            return cpu.s.gpr[kGpOrder[gp++]];
        }
        const u64 v = cpu.Load<u64>(stack);
        stack += 8;
        return v;
    }
    Vec NextVec() {
        if (vec < 8) {
            return cpu.s.ymm[vec++];
        }
        Vec v{};
        std::memcpy(&v, cpu.Ptr(stack, 16), 16);
        stack += 16;
        return v;
    }

    template <typename T>
    T Get() {
        using U = std::remove_cvref_t<T>;
        if constexpr (std::is_reference_v<T>) {
            return *reinterpret_cast<std::remove_reference_t<T>*>(NextGp());
        } else if constexpr (std::is_same_v<U, float>) {
            return NextVec().f[0];
        } else if constexpr (std::is_same_v<U, double>) {
            return NextVec().fd[0];
        } else if constexpr (kIsVector<U>) {
            const Vec v = NextVec();
            U out;
            std::memcpy(&out, &v, sizeof(U));
            return out;
        } else if constexpr (std::is_pointer_v<U> || std::is_member_pointer_v<U>) {
            const u64 raw = NextGp();
            U out;
            std::memcpy(&out, &raw, sizeof(U));
            return out;
        } else if constexpr (std::is_same_v<U, bool>) {
            // Only the low byte of a bool argument register is defined.
            return (NextGp() & 0xFF) != 0;
        } else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
            // Narrower-than-64-bit arguments occupy the low bytes of their register; the rest is
            // unspecified garbage, so take exactly sizeof(U) bytes.
            u64 parts[2] = {NextGp(), 0};
            if constexpr (sizeof(U) == 16) {
                parts[1] = NextGp();
            }
            U out;
            std::memcpy(&out, parts, sizeof(U));
            return out;
        } else {
            static_assert(std::is_trivially_copyable_v<U>, "HLE argument type can't be passed");
            U out;
            if constexpr (sizeof(U) <= 16) {
                u64 parts[2] = {NextGp(), sizeof(U) > 8 ? NextGp() : 0};
                std::memcpy(&out, parts, sizeof(U));
            } else {
                // MEMORY class: passed by value on the stack, 8-byte aligned.
                std::memcpy(&out, cpu.Ptr(stack, sizeof(U)), sizeof(U));
                stack += (sizeof(U) + 7) & ~u64{7};
            }
            return out;
        }
    }
};

template <typename T>
constexpr bool IsSignedLike() {
    if constexpr (std::is_enum_v<T>) {
        return std::is_signed_v<std::underlying_type_t<T>>;
    } else {
        return std::is_signed_v<T>;
    }
}

template <typename R>
void SetReturn(Cpu& cpu, const R& r) {
    using U = std::remove_cvref_t<R>;
    if constexpr (std::is_same_v<U, float>) {
        cpu.s.ymm[0] = {};
        cpu.s.ymm[0].f[0] = r;
    } else if constexpr (std::is_same_v<U, double>) {
        cpu.s.ymm[0] = {};
        cpu.s.ymm[0].fd[0] = r;
    } else if constexpr (kIsVector<U>) {
        cpu.s.ymm[0] = {};
        std::memcpy(&cpu.s.ymm[0], &r, 16);
    } else if constexpr (std::is_pointer_v<U>) {
        cpu.s.gpr[RAX] = reinterpret_cast<u64>(r);
    } else if constexpr (std::is_integral_v<U> || std::is_enum_v<U>) {
        if constexpr (sizeof(U) == 16) {
            u64 parts[2];
            std::memcpy(parts, &r, 16);
            cpu.s.gpr[RAX] = parts[0];
            cpu.s.gpr[RDX] = parts[1];
        } else if constexpr (IsSignedLike<U>()) {
            cpu.s.gpr[RAX] = static_cast<u64>(static_cast<s64>(r));
        } else {
            cpu.s.gpr[RAX] = static_cast<u64>(r);
        }
    } else {
        static_assert(sizeof(U) <= 16 && std::is_trivially_copyable_v<U>,
                      "struct returns larger than 16 bytes are not marshalled");
        u64 parts[2]{};
        std::memcpy(parts, &r, sizeof(U));
        cpu.s.gpr[RAX] = parts[0];
        cpu.s.gpr[RDX] = parts[1];
    }
}

void EnterHle(Cpu& cpu, u64 stack_args, u64& saved);
void LeaveHle(u64 saved);

} // namespace Abi

template <auto func>
struct HleThunk;

template <class R, class... Args, R (*func)(Args...)>
struct HleThunk<func> {
    static void Invoke(Cpu& cpu) {
        Abi::ArgReader reader{cpu, 0, 0, cpu.s.gpr[RSP] + 8};
        u64 saved;
        Abi::EnterHle(cpu, reader.stack, saved);
        // Braced initialization evaluates left to right, which is the order the ABI assigns
        // registers in.
        std::tuple<Args...> args{reader.template Get<Args>()...};
        if constexpr (std::is_void_v<R>) {
            std::apply(func, args);
        } else {
            const R result = std::apply(func, args);
            Abi::SetReturn(cpu, result);
        }
        Abi::LeaveHle(saved);
    }

    static u64 Address() {
        static const u64 addr = RegisterThunk(&Invoke, __PRETTY_FUNCTION__);
        return addr;
    }
};

// Same for variadic C functions (int f(const char*, ...)): only the fixed parameters are read;
// the function body reaches the rest through the guest's registers via VA_CTX.
template <class R, class... Args, R (*func)(Args..., ...)>
struct HleThunk<func> {
    static void Invoke(Cpu& cpu) {
        Abi::ArgReader reader{cpu, 0, 0, cpu.s.gpr[RSP] + 8};
        u64 saved;
        Abi::EnterHle(cpu, reader.stack, saved);
        std::tuple<Args...> args{reader.template Get<Args>()...};
        if constexpr (std::is_void_v<R>) {
            std::apply([](auto... a) { func(a...); }, args);
        } else {
            const R result = std::apply([](auto... a) { return func(a...); }, args);
            Abi::SetReturn(cpu, result);
        }
        Abi::LeaveHle(saved);
    }

    static u64 Address() {
        static const u64 addr = RegisterThunk(&Invoke, __PRETTY_FUNCTION__);
        return addr;
    }
};

// A function pointer of `func`'s own type whose value is its thunk address - what HOST_CALL
// yields, so callers that store it as a typed guest function pointer keep compiling.
template <auto func>
decltype(func) HleAddress() {
    const u64 addr = HleThunk<func>::Address();
    decltype(func) out;
    std::memcpy(&out, &addr, sizeof(out));
    return out;
}

} // namespace Core::Interpreter
