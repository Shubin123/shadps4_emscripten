// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include "core/interpreter/x86_register_file.h"

namespace Core::Interpreter {

namespace {

[[noreturn]] void UnhandledRegister(ZydisRegister reg) {
    std::fprintf(stderr, "Core::Interpreter: unhandled register id %d (ymm/zmm and registers "
                          "above xmm15 are not modeled yet)\n",
                  static_cast<int>(reg));
    std::abort();
}

// AL,CL,DL,BL,AH,CH,DH,BH,SPL,BPL,SIL,DIL,R8B..R15B - 20 consecutive Zydis enum values.
constexpr ZydisRegister kGpr8First = ZYDIS_REGISTER_AL;
constexpr ZydisRegister kGpr8Last = ZYDIS_REGISTER_R15B;
// AX,CX,DX,BX,SP,BP,SI,DI,R8W..R15W - 16 consecutive values, same order as the Gpr enum.
constexpr ZydisRegister kGpr16First = ZYDIS_REGISTER_AX;
constexpr ZydisRegister kGpr16Last = ZYDIS_REGISTER_R15W;
constexpr ZydisRegister kGpr32First = ZYDIS_REGISTER_EAX;
constexpr ZydisRegister kGpr32Last = ZYDIS_REGISTER_R15D;
constexpr ZydisRegister kGpr64First = ZYDIS_REGISTER_RAX;
constexpr ZydisRegister kGpr64Last = ZYDIS_REGISTER_R15;

struct Decoded {
    std::size_t index; // Gpr enum value (0..15)
    int width;          // 1, 2, 4 or 8 bytes
    bool high_byte;      // true for AH/CH/DH/BH
};

Decoded DecodeGpr(ZydisRegister reg) {
    if (reg >= kGpr8First && reg <= kGpr8Last) {
        const int pos = static_cast<int>(reg) - static_cast<int>(kGpr8First);
        if (pos < 4) {
            return {static_cast<std::size_t>(pos), 1, false}; // AL,CL,DL,BL
        }
        if (pos < 8) {
            return {static_cast<std::size_t>(pos - 4), 1, true}; // AH,CH,DH,BH
        }
        return {static_cast<std::size_t>(pos - 4), 1, false}; // SPL,BPL,SIL,DIL,R8B..R15B
    }
    if (reg >= kGpr16First && reg <= kGpr16Last) {
        return {static_cast<std::size_t>(reg - kGpr16First), 2, false};
    }
    if (reg >= kGpr32First && reg <= kGpr32Last) {
        return {static_cast<std::size_t>(reg - kGpr32First), 4, false};
    }
    if (reg >= kGpr64First && reg <= kGpr64Last) {
        return {static_cast<std::size_t>(reg - kGpr64First), 8, false};
    }
    UnhandledRegister(reg);
}

} // namespace

std::uint64_t CpuState::ReadGpr(ZydisRegister reg) const {
    const Decoded d = DecodeGpr(reg);
    const std::uint64_t full = gpr[d.index];
    if (d.high_byte) {
        return (full >> 8) & 0xFF;
    }
    switch (d.width) {
    case 1:
        return full & 0xFFull;
    case 2:
        return full & 0xFFFFull;
    case 4:
        return full & 0xFFFF'FFFFull;
    default:
        return full;
    }
}

void CpuState::WriteGpr(ZydisRegister reg, std::uint64_t value) {
    const Decoded d = DecodeGpr(reg);
    std::uint64_t& full = gpr[d.index];
    if (d.high_byte) {
        full = (full & ~0xFF00ull) | ((value & 0xFF) << 8);
        return;
    }
    switch (d.width) {
    case 1:
        full = (full & ~0xFFull) | (value & 0xFFull);
        break;
    case 2:
        full = (full & ~0xFFFFull) | (value & 0xFFFFull);
        break;
    case 4:
        // A write to a 32-bit GPR name zero-extends into the full 64-bit register. This is a
        // real x86-64 rule (not an x86-32 compatibility quirk) and guest code relies on it.
        full = value & 0xFFFF'FFFFull;
        break;
    default:
        full = value;
        break;
    }
}

const Xmm& CpuState::ReadXmm(ZydisRegister reg) const {
    if (reg < ZYDIS_REGISTER_XMM0 || reg > ZYDIS_REGISTER_XMM15) {
        UnhandledRegister(reg);
    }
    return xmm[reg - ZYDIS_REGISTER_XMM0];
}

void CpuState::WriteXmm(ZydisRegister reg, const Xmm& value) {
    if (reg < ZYDIS_REGISTER_XMM0 || reg > ZYDIS_REGISTER_XMM15) {
        UnhandledRegister(reg);
    }
    xmm[reg - ZYDIS_REGISTER_XMM0] = value;
}

} // namespace Core::Interpreter
