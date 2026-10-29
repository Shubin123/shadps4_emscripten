// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/interpreter/guest.h"
#include "core/interpreter/hle.h"

namespace Core::Interpreter {

namespace {

// Guest stack for a host thread that calls into guest code without already being inside it (the
// linker running module initializers, a host-created helper thread, ...). Natively such calls
// run on the host thread's own stack; here the guest needs a stack in addressable memory, and the
// interpreter's host C stack isn't one guest code can be pointed at.
constexpr u64 kHostThreadGuestStack = 1ULL << 20;
thread_local u8* tls_guest_stack = nullptr;
thread_local int tls_depth = 0;

u64 HostThreadStackTop() {
    if (!tls_guest_stack) {
        tls_guest_stack = static_cast<u8*>(std::aligned_alloc(64, kHostThreadGuestStack));
        ASSERT(tls_guest_stack);
    }
    return reinterpret_cast<u64>(tls_guest_stack) + kHostThreadGuestStack;
}

void SetArgs(CpuState& s, const GuestArgs& args) {
    static constexpr u8 order[6] = {RDI, RSI, RDX, RCX, R8, R9};
    ASSERT_MSG(args.ngp <= 6 && args.nvec <= 8, "guest call with stack-passed arguments");
    for (int i = 0; i < args.ngp; i++) {
        s.gpr[order[i]] = args.gp[i];
    }
    for (int i = 0; i < args.nvec; i++) {
        s.ymm[i] = args.vec[i];
    }
}

void ReportFault(const GuestFault& e) {
    LOG_CRITICAL(Core, "guest fault: {}", e.what());
    DumpCurrentState("fault");
}

} // namespace

GuestResult CallGuestRaw(u64 addr, const GuestArgs& args) {
    Cpu& cpu = Cpu::Current();
    const CpuState saved = cpu.s;
    CpuState& s = cpu.s;

    u64 sp;
    if (tls_depth > 0) {
        // Nested inside guest code (an HLE function calling back out): continue below the
        // guest's current frame, past the 128-byte red zone the ABI lets leaf code use.
        sp = s.gpr[RSP] - 128;
    } else {
        sp = HostThreadStackTop();
    }
    sp &= ~u64{15};
    s.gpr[RSP] = sp;
    SetArgs(s, args);
    s.gpr[RAX] = static_cast<u64>(args.nvec); // what a varargs callee expects in al
    cpu.Push64(kReturnSentinel);
    s.rip = addr;

    tls_depth++;
    try {
        cpu.RunUntil(kReturnSentinel);
    } catch (const GuestFault& e) {
        tls_depth--;
        ReportFault(e);
        throw;
    }
    tls_depth--;

    const GuestResult r{s.gpr[RAX], s.gpr[RDX], s.ymm[0]};
    // Everything but the result is restored: the callee is entitled to clobber caller-saved
    // registers, and the host code that called it has no notion of guest register state anyway.
    const u64 fs = s.fs_base;
    s = saved;
    s.fs_base = fs;
    return r;
}

u64 RunOnGuestStack(u64 func, u64 arg, u64 stack_top) {
    if (IsHostFunction(func)) {
        using HostFn = void* (*)(void*);
        HostFn fn;
        std::memcpy(&fn, &func, sizeof(fn));
        return reinterpret_cast<u64>(fn(reinterpret_cast<void*>(arg)));
    }
    Cpu& cpu = Cpu::Current();
    CpuState& s = cpu.s;
    s.gpr[RSP] = stack_top & ~u64{15};
    s.gpr[RDI] = arg;
    cpu.Push64(kReturnSentinel);
    s.rip = func;
    tls_depth++;
    try {
        cpu.RunUntil(kReturnSentinel);
    } catch (const GuestFault& e) {
        tls_depth--;
        ReportFault(e);
        throw;
    }
    tls_depth--;
    return s.gpr[RAX];
}

void RunMainEntry(u64 entry, u64 params, u64 exit_func) {
    // Native shadPS4 runs the entry on the main game thread's own (8MB-class) host stack.
    constexpr u64 kMainStackSize = 8ULL << 20;
    u8* stack = static_cast<u8*>(std::aligned_alloc(64, kMainStackSize));
    ASSERT(stack);
    Cpu& cpu = Cpu::Current();
    CpuState& s = cpu.s;

    // Mirrors the inline asm in Linker::RunMainEntry: align, misalign by 8 (videoout_basic
    // expects it), push the two words of EntryParams the way the PS4 kernel lays them out, then
    // enter with rdi = params, rsi = exit function. There is no return.
    u64 sp = (reinterpret_cast<u64>(stack) + kMainStackSize) & ~u64{15};
    sp -= 8;
    s.gpr[RSP] = sp;
    cpu.Push64(cpu.Load<u64>(params + 8));
    cpu.Push64(cpu.Load<u64>(params + 0));
    s.gpr[RDI] = params;
    s.gpr[RSI] = exit_func;
    s.rip = entry;
    tls_depth++;
    try {
        cpu.RunUntil(kReturnSentinel);
    } catch (const GuestFault& e) {
        ReportFault(e);
        LOG_CRITICAL(Core, "main thread stopped after {} guest instructions",
                     cpu.InstructionsExecuted());
        Common::Log::Flush();
        std::quick_exit(1);
    }
    UNREACHABLE_MSG("guest entry point returned");
}

void DumpCurrentState(const char* why) {
    Cpu* cpu = Cpu::CurrentIfAny();
    if (!cpu) {
        return;
    }
    const CpuState& s = cpu->s;
    static constexpr const char* names[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                                              "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                                              "r12", "r13", "r14", "r15"};
    std::string regs;
    for (int i = 0; i < 16; i++) {
        regs += fmt::format("{}={:#x} ", names[i], s.gpr[i]);
    }
    LOG_CRITICAL(Core, "[{}] rip={:#x} rflags={:#x} fs={:#x} insns={}", why, s.rip, s.rflags,
                 s.fs_base, cpu->InstructionsExecuted());
    LOG_CRITICAL(Core, "[{}] {}", why, regs);
    LOG_CRITICAL(Core, "[{}] at rip: {}", why, Disassemble(s.rip));
    // Return addresses near the top of the stack, for a rough guest backtrace.
    std::string trail;
    for (int i = 0; i < 16; i++) {
        const u64 a = s.gpr[RSP] + i * 8;
        if (a + 8 > cpu->mem_limit) {
            break;
        }
        u64 v;
        std::memcpy(&v, reinterpret_cast<const void*>(a), 8);
        if (v >= 0x400000 && v < 0x300000000ULL) {
            trail += fmt::format("{:#x} ", v);
        }
    }
    LOG_CRITICAL(Core, "[{}] code-looking stack words: {}", why, trail);
}

} // namespace Core::Interpreter
