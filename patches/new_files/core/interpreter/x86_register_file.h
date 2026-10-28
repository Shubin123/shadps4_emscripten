// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <Zydis/Zydis.h>

namespace Core::Interpreter {

// General-purpose register indices, matching the x86-64 encoding order used by Zydis
// (ZYDIS_REGISTER_RAX..ZYDIS_REGISTER_R15 are contiguous in this order).
enum class Gpr : std::uint8_t {
    Rax = 0,
    Rcx,
    Rdx,
    Rbx,
    Rsp,
    Rbp,
    Rsi,
    Rdi,
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    Count,
};

// RFLAGS bits actually tracked. Reserved/system bits (IOPL, IF, TF, ...) are not needed for
// interpreting user-mode guest code and are kept zeroed.
namespace Rflags {
constexpr std::uint64_t CF = 1ull << 0;
constexpr std::uint64_t PF = 1ull << 2;
constexpr std::uint64_t AF = 1ull << 4;
constexpr std::uint64_t ZF = 1ull << 6;
constexpr std::uint64_t SF = 1ull << 7;
constexpr std::uint64_t OF = 1ull << 11;
} // namespace Rflags

struct Xmm {
    std::uint8_t bytes[16];
};

// Full interpreted-CPU state for one guest thread. shadPS4 runs one Core::Interpreter::Cpu per
// guest thread (see core/thread.cpp integration notes in PROJECT.md); nothing here is shared
// between threads, matching the real CPU's per-core register file.
struct CpuState {
    std::array<std::uint64_t, static_cast<std::size_t>(Gpr::Count)> gpr{};
    std::uint64_t rip = 0;
    std::uint64_t rflags = 0;
    std::array<Xmm, 16> xmm{};

    // FS/GS segment bases. Direct execution gets these from the real CPU's FSBASE/GSBASE MSRs
    // (set up per-thread in core/tls.cpp for the guest's TLS pointer); the interpreter has no
    // real segment registers to read, so EffectiveAddress() adds these explicitly whenever an
    // operand carries a non-default segment prefix. Wiring tls.cpp to populate this per guest
    // thread is part of the integration milestone, not done yet - see PROJECT.md.
    std::uint64_t fs_base = 0;
    std::uint64_t gs_base = 0;

    std::uint64_t& Reg(Gpr r) {
        return gpr[static_cast<std::size_t>(r)];
    }

    // Reads a GPR as interpreted by Zydis's register enum, honoring the x86-64 sub-register
    // aliasing rules (8/16/32/64-bit views, and the legacy AH/CH/DH/BH high-byte registers).
    std::uint64_t ReadGpr(ZydisRegister reg) const;

    // Writes a GPR per the aliasing rules above. A 32-bit write zero-extends into the full
    // 64-bit register (standard x86-64 behavior); 8/16-bit writes preserve the upper bits.
    void WriteGpr(ZydisRegister reg, std::uint64_t value);

    const Xmm& ReadXmm(ZydisRegister reg) const;
    void WriteXmm(ZydisRegister reg, const Xmm& value);

    bool Flag(std::uint64_t mask) const {
        return (rflags & mask) != 0;
    }
    void SetFlag(std::uint64_t mask, bool set) {
        rflags = set ? (rflags | mask) : (rflags & ~mask);
    }
};

} // namespace Core::Interpreter
