// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// x86-64 interpreter for the wasm build.
//
// Natively, shadPS4 executes the game's x86-64 code directly on the host CPU. WebAssembly can't
// execute machine code it is handed, so guest code is interpreted here instead: decoded with
// Zydis (already a shadPS4 dependency), cached per instruction address, and executed against a
// CpuState. Guest memory is accessed at its own addresses (core/wasm_layout.h explains why that
// works inside wasm linear memory), so a guest pointer handed to HLE code needs no translation.
//
// One Cpu exists per host thread that runs guest code (Cpu::Current()). HLE functions are reached
// through synthetic addresses (core/interpreter/hle.h); guest functions are called from host code
// with Cpu::Call (core/interpreter/guest.h).
//
// Coverage targets what the PS4's AMD Jaguar implements - x86-64 integer, x87, SSE through SSE4.2,
// AVX (no AVX2/FMA), F16C, BMI1, LZCNT/POPCNT, MOVBE, AES-NI/PCLMULQDQ. Anything not implemented
// raises GuestFault with the mnemonic and address rather than guessing.

#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <Zydis/Zydis.h>
#include "common/types.h"

namespace Core::Interpreter {

union alignas(32) Vec {
    u8 b[32];
    s8 sb[32];
    u16 w[16];
    s16 sw[16];
    u32 d[8];
    s32 sd[8];
    u64 q[4];
    s64 sq[4];
    float f[8];
    double fd[4];
};
static_assert(sizeof(Vec) == 32);

// RFLAGS bits the interpreter tracks.
enum Flag : u64 {
    CF = 1ULL << 0,
    PF = 1ULL << 2,
    AF = 1ULL << 4,
    ZF = 1ULL << 6,
    SF = 1ULL << 7,
    TF = 1ULL << 8,
    IF = 1ULL << 9,
    DF = 1ULL << 10,
    OF = 1ULL << 11,
};
constexpr u64 kStatusFlags = CF | PF | AF | ZF | SF | OF;

struct CpuState {
    std::array<u64, 16> gpr{}; // rax rcx rdx rbx rsp rbp rsi rdi r8..r15 (x86 encoding order)
    u64 rip{};
    u64 rflags{0x202};
    u64 fs_base{};
    u64 gs_base{};
    std::array<Vec, 16> ymm{};
    u32 mxcsr{0x1F80};

    // x87, kept in double precision (the PS4 SDK's code is SSE-based; x87 shows up mostly in
    // libm corner cases and control-word fiddling).
    std::array<double, 8> st{};
    u16 fpu_cw{0x037F};
    u16 fpu_sw{};
    u8 fpu_top{};
    u8 fpu_tag_empty{0xFF}; // bit i set = physical register i empty
};

enum Gpr : u8 { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

// Register operand encoding inside a decoded instruction.
//   0..15   GPR (by Gpr index); kHighByte flag selects AH/CH/DH/BH
//   32..47  XMM/YMM (size of the operand says which)
//   64..71  x87 ST(i)
//   80..87  MMX (unsupported; decoded so the fault message names them)
//   96+     segment registers (96 + ES/CS/SS/DS/FS/GS)
constexpr u8 kHighByte = 0x10;
constexpr u8 kRegVec = 32;
constexpr u8 kRegSt = 64;
constexpr u8 kRegMmx = 80;
constexpr u8 kRegSeg = 96;
constexpr u8 kRegNone = 0xFF;
constexpr u8 kRegRip = 0xFE;

enum class OpKind : u8 { None, Reg, Mem, Imm };

struct Op {
    OpKind kind{OpKind::None};
    u8 size{}; // bytes
    u8 reg{kRegNone};
    u8 base{kRegNone};
    u8 index{kRegNone};
    u8 scale{};
    u8 seg{}; // 0 = none, 1 = fs, 2 = gs
    bool imm_signed{};
    s64 value{}; // displacement (Mem) or immediate (Imm)
};

enum InsnAttr : u16 {
    kLock = 1 << 0,
    kRep = 1 << 1,   // REP / REPE / REPZ
    kRepne = 1 << 2, // REPNE / REPNZ
    kVex = 1 << 3,
    kVex256 = 1 << 4,
    kAddr32 = 1 << 5,
};

struct Insn {
    u16 mnemonic{};   // ZydisMnemonic, with VEX forms folded onto their SSE name (see decode.cpp)
    u16 raw_mnemonic{}; // the mnemonic as decoded
    u8 length{};
    u8 nops{};       // visible operands
    u8 opsize{};     // operand width in bytes (ZydisDecodedInstruction::operand_width / 8)
    u16 attrs{};
    std::array<Op, 4> op{};
};

struct GuestFault : std::runtime_error {
    explicit GuestFault(const std::string& what) : std::runtime_error(what) {}
};

class Cpu {
public:
    Cpu();
    ~Cpu();

    // The interpreter for the calling host thread, created on first use.
    static Cpu& Current();
    // Null if the calling thread has never run guest code.
    static Cpu* CurrentIfAny();

    CpuState& State() {
        return s;
    }

    // Runs guest code from s.rip until it returns to `return_address` (a sentinel pushed by the
    // caller) - see Call() in guest.h for the calling-convention wrapper.
    void RunUntil(u64 return_address);

    u64 InstructionsExecuted() const {
        return icount;
    }

    // Drops cached decodes for a range whose code may have changed (module unload, JIT).
    void InvalidateCode(u64 addr, u64 size);

    [[noreturn]] void Fault(const char* what, u64 addr = 0) const;

    // Guest memory access. Guest addresses are host addresses (core/wasm_layout.h); these only
    // add the bounds check that turns a wild guest pointer into a GuestFault instead of silently
    // corrupting the emulator's own memory.
    template <typename T>
    T Load(u64 addr) {
        T v;
        std::memcpy(&v, Ptr(addr, sizeof(T)), sizeof(T));
        return v;
    }
    template <typename T>
    void Store(u64 addr, T v) {
        std::memcpy(Ptr(addr, sizeof(T)), &v, sizeof(T));
    }
    u8* Ptr(u64 addr, u64 size) {
        if (addr < kMinAddress || addr + size > mem_limit) [[unlikely]] {
            return SlowPtr(addr, size);
        }
        return reinterpret_cast<u8*>(addr);
    }

    void Push64(u64 v) {
        s.gpr[RSP] -= 8;
        Store<u64>(s.gpr[RSP], v);
    }
    u64 Pop64() {
        const u64 v = Load<u64>(s.gpr[RSP]);
        s.gpr[RSP] += 8;
        return v;
    }

    const Insn* Decode(u64 rip);

private:
    static constexpr u64 kMinAddress = 0x10000;
    u8* SlowPtr(u64 addr, u64 size);

    void Step(const Insn& in);
    bool ExecInteger(const Insn& in);
    bool ExecString(const Insn& in);
    bool ExecX87(const Insn& in);
    bool ExecSimd(const Insn& in);
    [[noreturn]] void Unimplemented(const Insn& in) const;

public:
    // Operand helpers, shared by the exec_*.cpp files.
    u64 EffectiveAddress(const Op& o) const;
    u64 ReadGpr(u8 reg, u8 size) const;
    void WriteGpr(u8 reg, u8 size, u64 v);
    u64 Read(const Op& o);
    void Write(const Op& o, u64 v);
    Vec ReadVec(const Op& o);
    void WriteVec(const Insn& in, const Op& o, const Vec& v, u8 size);
    bool Cond(int cc) const;

    void SetSzp(u64 r, u8 size);
    u64 FlagsAdd(u64 a, u64 b, u64 carry, u8 size);
    u64 FlagsSub(u64 a, u64 b, u64 borrow, u8 size);
    u64 FlagsLogic(u64 r, u8 size);

    CpuState s;
    u64 next_rip{};
    u64 icount{};
    u64 mem_limit{};

private:
    struct CacheEntry {
        u64 rip{~0ULL};
        u64 bytes[2]{};
        Insn insn;
    };
    static constexpr size_t kCacheBits = 16;
    std::unique_ptr<CacheEntry[]> cache;
    ZydisDecoder decoder;
};

// Mnemonic table built once: maps VEX mnemonics (VADDPS) to their SSE counterpart (ADDPS) so one
// handler serves both encodings.
u16 FoldMnemonic(ZydisMnemonic m);

std::string Disassemble(u64 addr);

} // namespace Core::Interpreter
