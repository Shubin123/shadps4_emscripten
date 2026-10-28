// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstdlib>
#include "core/interpreter/x86_interpreter.h"

namespace Core::Interpreter {

namespace {

std::uint64_t MaskFor(int width) {
    switch (width) {
    case 1:
        return 0xFFull;
    case 2:
        return 0xFFFFull;
    case 4:
        return 0xFFFF'FFFFull;
    default:
        return ~0ull;
    }
}

std::uint64_t SignBitFor(int width) {
    return 1ull << (width * 8 - 1);
}

bool Parity(std::uint8_t value) {
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return (value & 1) == 0;
}

std::int64_t SignExtend(std::uint64_t value, int width) {
    switch (width) {
    case 1:
        return static_cast<std::int8_t>(value);
    case 2:
        return static_cast<std::int16_t>(value);
    case 4:
        return static_cast<std::int32_t>(value);
    default:
        return static_cast<std::int64_t>(value);
    }
}

// Maps a GPR width (1/2/4/8 bytes) and family position (0 for the RAX family, 2 for the RDX
// family - matching Gpr::Rax == 0, Gpr::Rdx == 2) to the matching Zydis register name, e.g.
// GprByWidth(4, 2) == EDX. Relies on AL/AX/EAX/RAX, ..., DL/DX/EDX/RDX, ... each being 4
// contiguous same-order ranges in the Zydis register enum (see x86_register_file.cpp).
ZydisRegister GprByWidth(int width, int pos) {
    switch (width) {
    case 1:
        return static_cast<ZydisRegister>(ZYDIS_REGISTER_AL + pos);
    case 2:
        return static_cast<ZydisRegister>(ZYDIS_REGISTER_AX + pos);
    case 4:
        return static_cast<ZydisRegister>(ZYDIS_REGISTER_EAX + pos);
    default:
        return static_cast<ZydisRegister>(ZYDIS_REGISTER_RAX + pos);
    }
}

[[noreturn]] void Fatal(std::uint64_t address, const char* message) {
    std::fprintf(stderr, "Core::Interpreter: %s (at %#llx)\n", message,
                  static_cast<unsigned long long>(address));
    std::abort();
}

} // namespace

X86Interpreter::X86Interpreter(GuestMemory& memory_, HostThunkTable& thunks_)
    : memory{memory_}, thunks{thunks_} {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
}

void X86Interpreter::Unimplemented(const ZydisDecodedInstruction& instr) const {
    const char* name = ZydisMnemonicGetString(instr.mnemonic);
    std::fprintf(stderr,
                  "Core::Interpreter: unimplemented instruction '%s' at %#llx - see PROJECT.md, "
                  "'Instruction coverage'\n",
                  name ? name : "?", static_cast<unsigned long long>(current_instr_address));
    std::abort();
}

int X86Interpreter::OperandWidthBytes(const ZydisDecodedOperand& op) const {
    return static_cast<int>(op.size / 8);
}

std::uint64_t X86Interpreter::EffectiveAddress(const ZydisDecodedOperand& op,
                                                const ZydisDecodedInstruction& instr) const {
    // Deliberately not using Zydis's ZydisCalcAbsoluteAddress() here: per its own header
    // documentation (Zydis/Utils.h), that function only resolves IMM-relative, RIP-relative and
    // literal-absolute operands - it has no access to the CPU's actual register values, so it
    // cannot and does not compute general base/index-register addressing (plain
    // ZydisRegisterContext-less `[rdi]`, `[rsp+8]`, `[rax+rbx*4+0x10]`, ...), which is most
    // memory operands in practice. (ZydisCalcAbsoluteAddressEx takes a register-context and
    // could do this, but building a full ZydisRegisterContext just to read 1-2 registers is
    // more indirection than just computing it directly here.)
    std::uint64_t address = 0;
    if (op.mem.base == ZYDIS_REGISTER_RIP || op.mem.base == ZYDIS_REGISTER_EIP) {
        // RIP-relative addressing is defined relative to the address of the *next* instruction.
        address = current_instr_address + instr.length;
    } else if (op.mem.base != ZYDIS_REGISTER_NONE) {
        address = state.ReadGpr(op.mem.base);
    }
    if (op.mem.index != ZYDIS_REGISTER_NONE) {
        address += state.ReadGpr(op.mem.index) * op.mem.scale;
    }
    address += static_cast<std::uint64_t>(op.mem.disp.value);
    // FS/GS segment bases are a runtime concept (the thread's TLS base via the FSBASE/GSBASE
    // MSRs on real hardware) - nothing in the instruction encoding gives us this, and nothing
    // populates state.fs_base/gs_base yet (core/tls.cpp integration is not done - see
    // PROJECT.md), so guest TLS access will silently use a base of 0 until that's wired up.
    if (op.mem.segment == ZYDIS_REGISTER_FS) {
        address += state.fs_base;
    } else if (op.mem.segment == ZYDIS_REGISTER_GS) {
        address += state.gs_base;
    }
    return address;
}

std::uint64_t X86Interpreter::ResolveBranchTarget(const ZydisDecodedOperand& op,
                                                   const ZydisDecodedInstruction& instr) {
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
        // Relative branch displacements ARE one of the cases ZydisCalcAbsoluteAddress is
        // documented to handle correctly (it needs no register value, only
        // current_instr_address and the encoded displacement), unlike general memory operands.
        ZyanU64 address = 0;
        ZydisCalcAbsoluteAddress(&instr, &op, current_instr_address, &address);
        return address;
    }
    case ZYDIS_OPERAND_TYPE_REGISTER:
        return state.ReadGpr(op.reg.value);
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return memory.Read(EffectiveAddress(op, instr), OperandWidthBytes(op));
    default:
        Unimplemented(instr);
    }
}

std::uint64_t X86Interpreter::ReadOperand(const ZydisDecodedOperand& op,
                                           const ZydisDecodedInstruction& instr) {
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        return state.ReadGpr(op.reg.value);
    case ZYDIS_OPERAND_TYPE_MEMORY:
        return memory.Read(EffectiveAddress(op, instr), OperandWidthBytes(op));
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        return op.imm.is_signed ? static_cast<std::uint64_t>(op.imm.value.s) : op.imm.value.u;
    default:
        Unimplemented(instr);
    }
}

void X86Interpreter::WriteOperand(const ZydisDecodedOperand& op,
                                   const ZydisDecodedInstruction& instr, std::uint64_t value) {
    switch (op.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        state.WriteGpr(op.reg.value, value);
        return;
    case ZYDIS_OPERAND_TYPE_MEMORY:
        memory.Write(EffectiveAddress(op, instr), value, OperandWidthBytes(op));
        return;
    default:
        Unimplemented(instr);
    }
}

void X86Interpreter::Push(std::uint64_t value, int width) {
    state.Reg(Gpr::Rsp) -= width;
    memory.Write(state.Reg(Gpr::Rsp), value, width);
}

std::uint64_t X86Interpreter::Pop(int width) {
    const std::uint64_t value = memory.Read(state.Reg(Gpr::Rsp), width);
    state.Reg(Gpr::Rsp) += width;
    return value;
}

void X86Interpreter::SetSzp(std::uint64_t result, int width) {
    const std::uint64_t masked = result & MaskFor(width);
    state.SetFlag(Rflags::ZF, masked == 0);
    state.SetFlag(Rflags::SF, (masked & SignBitFor(width)) != 0);
    state.SetFlag(Rflags::PF, Parity(static_cast<std::uint8_t>(masked & 0xFF)));
}

std::uint64_t X86Interpreter::UpdateFlagsAdd(std::uint64_t lhs, std::uint64_t rhs, int width,
                                              std::uint64_t carry_in) {
    const std::uint64_t mask = MaskFor(width);
    const std::uint64_t sign = SignBitFor(width);
    const std::uint64_t l = lhs & mask;
    const std::uint64_t r = rhs & mask;
    const unsigned __int128 wide =
        static_cast<unsigned __int128>(l) + static_cast<unsigned __int128>(r) + carry_in;
    const std::uint64_t result = static_cast<std::uint64_t>(wide) & mask;
    const bool cf = (wide >> (width * 8)) != 0;
    const bool lhs_sign = (l & sign) != 0;
    const bool rhs_sign = (r & sign) != 0;
    const bool res_sign = (result & sign) != 0;
    const bool of = (lhs_sign == rhs_sign) && (res_sign != lhs_sign);
    const bool af = (((l & 0xF) + (r & 0xF) + carry_in) & 0x10) != 0;
    state.SetFlag(Rflags::CF, cf);
    state.SetFlag(Rflags::OF, of);
    state.SetFlag(Rflags::AF, af);
    SetSzp(result, width);
    return result;
}

std::uint64_t X86Interpreter::UpdateFlagsSub(std::uint64_t lhs, std::uint64_t rhs, int width,
                                              std::uint64_t borrow_in) {
    const std::uint64_t mask = MaskFor(width);
    const std::uint64_t sign = SignBitFor(width);
    const std::uint64_t l = lhs & mask;
    const std::uint64_t r = rhs & mask;
    const unsigned __int128 subtrahend = static_cast<unsigned __int128>(r) + borrow_in;
    const bool cf = static_cast<unsigned __int128>(l) < subtrahend;
    const std::uint64_t result =
        static_cast<std::uint64_t>(static_cast<unsigned __int128>(l) - subtrahend) & mask;
    const std::uint64_t rhs_mod = static_cast<std::uint64_t>(subtrahend) & mask;
    const bool lhs_sign = (l & sign) != 0;
    const bool rhs_sign = (rhs_mod & sign) != 0;
    const bool res_sign = (result & sign) != 0;
    const bool of = (lhs_sign != rhs_sign) && (res_sign != lhs_sign);
    const bool af = (l & 0xF) < ((r & 0xF) + borrow_in);
    state.SetFlag(Rflags::CF, cf);
    state.SetFlag(Rflags::OF, of);
    state.SetFlag(Rflags::AF, af);
    SetSzp(result, width);
    return result;
}

std::uint64_t X86Interpreter::UpdateFlagsLogic(std::uint64_t result, int width) {
    const std::uint64_t masked = result & MaskFor(width);
    SetSzp(masked, width);
    state.SetFlag(Rflags::CF, false);
    state.SetFlag(Rflags::OF, false);
    // AF is left untouched - real hardware leaves it undefined for logic ops too.
    return masked;
}

std::uint64_t X86Interpreter::UpdateFlagsShiftLeft(std::uint64_t original, std::uint64_t count,
                                                    int width) {
    const std::uint64_t mask = MaskFor(width);
    original &= mask;
    if (count == 0) {
        return original; // x86 leaves every flag untouched when the shift count is 0.
    }
    const int bits = width * 8;
    std::uint64_t result;
    bool cf;
    if (count >= static_cast<std::uint64_t>(bits)) {
        // Undefined by the manual for counts at/above the operand width; real chips vary. This
        // is a rare case to hit in practice (shifting a value out of existence), so the exact
        // flag values here are an approximation rather than a researched match to real silicon.
        result = 0;
        cf = false;
    } else {
        result = (original << count) & mask;
        cf = ((original >> (bits - static_cast<int>(count))) & 1) != 0;
    }
    SetSzp(result, width);
    state.SetFlag(Rflags::CF, cf);
    if (count == 1) {
        // OF is only architecturally defined for a single-bit shift.
        state.SetFlag(Rflags::OF, ((result & SignBitFor(width)) != 0) != cf);
    }
    return result;
}

std::uint64_t X86Interpreter::UpdateFlagsShiftRight(std::uint64_t original, std::uint64_t count,
                                                     int width, bool arithmetic) {
    const std::uint64_t mask = MaskFor(width);
    original &= mask;
    if (count == 0) {
        return original;
    }
    const int bits = width * 8;
    const bool original_sign = (original & SignBitFor(width)) != 0;
    std::uint64_t result;
    bool cf;
    if (count >= static_cast<std::uint64_t>(bits)) {
        result = (arithmetic && original_sign) ? mask : 0;
        cf = arithmetic && original_sign;
    } else {
        cf = ((original >> (static_cast<int>(count) - 1)) & 1) != 0;
        if (arithmetic) {
            const std::int64_t signed_val = SignExtend(original, width);
            result = static_cast<std::uint64_t>(signed_val >> count) & mask;
        } else {
            result = (original >> count) & mask;
        }
    }
    SetSzp(result, width);
    state.SetFlag(Rflags::CF, cf);
    if (count == 1) {
        // SHR by 1: OF is the vacated sign bit. SAR by 1 can never overflow (sign is preserved).
        state.SetFlag(Rflags::OF, arithmetic ? false : original_sign);
    }
    return result;
}

int X86Interpreter::ConditionCodeOf(ZydisMnemonic mnemonic) const {
    switch (mnemonic) {
    case ZYDIS_MNEMONIC_JO:
    case ZYDIS_MNEMONIC_SETO:
    case ZYDIS_MNEMONIC_CMOVO:
        return 0;
    case ZYDIS_MNEMONIC_JNO:
    case ZYDIS_MNEMONIC_SETNO:
    case ZYDIS_MNEMONIC_CMOVNO:
        return 1;
    case ZYDIS_MNEMONIC_JB:
    case ZYDIS_MNEMONIC_SETB:
    case ZYDIS_MNEMONIC_CMOVB:
        return 2;
    case ZYDIS_MNEMONIC_JNB:
    case ZYDIS_MNEMONIC_SETNB:
    case ZYDIS_MNEMONIC_CMOVNB:
        return 3;
    case ZYDIS_MNEMONIC_JZ:
    case ZYDIS_MNEMONIC_SETZ:
    case ZYDIS_MNEMONIC_CMOVZ:
        return 4;
    case ZYDIS_MNEMONIC_JNZ:
    case ZYDIS_MNEMONIC_SETNZ:
    case ZYDIS_MNEMONIC_CMOVNZ:
        return 5;
    case ZYDIS_MNEMONIC_JBE:
    case ZYDIS_MNEMONIC_SETBE:
    case ZYDIS_MNEMONIC_CMOVBE:
        return 6;
    case ZYDIS_MNEMONIC_JNBE:
    case ZYDIS_MNEMONIC_SETNBE:
    case ZYDIS_MNEMONIC_CMOVNBE:
        return 7;
    case ZYDIS_MNEMONIC_JS:
    case ZYDIS_MNEMONIC_SETS:
    case ZYDIS_MNEMONIC_CMOVS:
        return 8;
    case ZYDIS_MNEMONIC_JNS:
    case ZYDIS_MNEMONIC_SETNS:
    case ZYDIS_MNEMONIC_CMOVNS:
        return 9;
    case ZYDIS_MNEMONIC_JP:
    case ZYDIS_MNEMONIC_SETP:
    case ZYDIS_MNEMONIC_CMOVP:
        return 10;
    case ZYDIS_MNEMONIC_JNP:
    case ZYDIS_MNEMONIC_SETNP:
    case ZYDIS_MNEMONIC_CMOVNP:
        return 11;
    case ZYDIS_MNEMONIC_JL:
    case ZYDIS_MNEMONIC_SETL:
    case ZYDIS_MNEMONIC_CMOVL:
        return 12;
    case ZYDIS_MNEMONIC_JNL:
    case ZYDIS_MNEMONIC_SETNL:
    case ZYDIS_MNEMONIC_CMOVNL:
        return 13;
    case ZYDIS_MNEMONIC_JLE:
    case ZYDIS_MNEMONIC_SETLE:
    case ZYDIS_MNEMONIC_CMOVLE:
        return 14;
    case ZYDIS_MNEMONIC_JNLE:
    case ZYDIS_MNEMONIC_SETNLE:
    case ZYDIS_MNEMONIC_CMOVNLE:
        return 15;
    default:
        return -1;
    }
}

bool X86Interpreter::EvaluateConditionCode(int cc) const {
    switch (cc) {
    case 0:
        return state.Flag(Rflags::OF);
    case 1:
        return !state.Flag(Rflags::OF);
    case 2:
        return state.Flag(Rflags::CF);
    case 3:
        return !state.Flag(Rflags::CF);
    case 4:
        return state.Flag(Rflags::ZF);
    case 5:
        return !state.Flag(Rflags::ZF);
    case 6:
        return state.Flag(Rflags::CF) || state.Flag(Rflags::ZF);
    case 7:
        return !state.Flag(Rflags::CF) && !state.Flag(Rflags::ZF);
    case 8:
        return state.Flag(Rflags::SF);
    case 9:
        return !state.Flag(Rflags::SF);
    case 10:
        return state.Flag(Rflags::PF);
    case 11:
        return !state.Flag(Rflags::PF);
    case 12:
        return state.Flag(Rflags::SF) != state.Flag(Rflags::OF);
    case 13:
        return state.Flag(Rflags::SF) == state.Flag(Rflags::OF);
    case 14:
        return (state.Flag(Rflags::SF) != state.Flag(Rflags::OF)) || state.Flag(Rflags::ZF);
    case 15:
        return (state.Flag(Rflags::SF) == state.Flag(Rflags::OF)) && !state.Flag(Rflags::ZF);
    default:
        return false;
    }
}

void X86Interpreter::Step() {
    std::uint8_t* code = memory.Translate(state.rip, ZYDIS_MAX_INSTRUCTION_LENGTH,
                                           /*for_write=*/false);
    if (!code) {
        Fatal(state.rip, "fetch from unmapped address");
    }
    ZydisDecodedInstruction instr;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    const ZyanStatus status =
        ZydisDecoderDecodeFull(&decoder, code, ZYDIS_MAX_INSTRUCTION_LENGTH, &instr, ops);
    if (!ZYAN_SUCCESS(status)) {
        Fatal(state.rip, "failed to decode instruction");
    }
    current_instr_address = state.rip;
    state.rip += instr.length;
    Execute(instr, ops);
}

void X86Interpreter::RunUntil(std::uint64_t stop_address) {
    while (state.rip != stop_address) {
        if (thunks.TryInvoke(state.rip, *this)) {
            // The guest reached this address the same way it reaches any other function: via a
            // `call` that already pushed a return address. Simulate the matching `ret` now that
            // the real host function has run and left its result in RAX.
            state.rip = Pop(8);
            continue;
        }
        Step();
    }
}

std::uint64_t X86Interpreter::CallGuestFunction(std::uint64_t function_address,
                                                 std::initializer_list<std::uint64_t> args) {
    static constexpr Gpr kSysvArgRegs[6] = {Gpr::Rdi, Gpr::Rsi, Gpr::Rdx,
                                             Gpr::Rcx, Gpr::R8,  Gpr::R9};
    if (args.size() > 6) {
        Fatal(function_address, "CallGuestFunction with more than 6 arguments is not "
                                 "implemented");
    }
    std::size_t i = 0;
    for (std::uint64_t arg : args) {
        state.Reg(kSysvArgRegs[i]) = arg;
        ++i;
    }
    Push(kCallSentinel, 8);
    state.rip = function_address;
    RunUntil(kCallSentinel);
    return state.Reg(Gpr::Rax);
}

void X86Interpreter::RunMainEntry(std::uint64_t entry_address, std::uint64_t params_address,
                                   std::uint64_t exit_address) {
    // Mirrors Core::Linker::RunMainEntry's asm block exactly. Assumes state.Reg(Gpr::Rsp) already
    // holds the initial guest stack pointer, same as the direct-execution path expects.
    state.Reg(Gpr::Rsp) &= ~0xFull; // andq $-16, %rsp
    state.Reg(Gpr::Rsp) -= 8;       // subq $8, %rsp
    const std::uint64_t field0 = memory.Read(params_address, 8);
    const std::uint64_t field1 = memory.Read(params_address + 8, 8);
    Push(field1, 8); // pushq 8(%1)
    Push(field0, 8); // pushq 0(%1)
    state.Reg(Gpr::Rdi) = params_address;
    state.Reg(Gpr::Rsi) = exit_address;
    state.rip = entry_address;
    RunUntil(kCallSentinel); // kCallSentinel is never produced on this path; see below.
    Fatal(state.rip, "RunMainEntry's run loop returned, which should never happen");
}

void X86Interpreter::Execute(const ZydisDecodedInstruction& instr,
                              const ZydisDecodedOperand* ops) {
    if (const int cc = ConditionCodeOf(instr.mnemonic); cc >= 0) {
        switch (instr.mnemonic) {
        case ZYDIS_MNEMONIC_JO:
        case ZYDIS_MNEMONIC_JNO:
        case ZYDIS_MNEMONIC_JB:
        case ZYDIS_MNEMONIC_JNB:
        case ZYDIS_MNEMONIC_JZ:
        case ZYDIS_MNEMONIC_JNZ:
        case ZYDIS_MNEMONIC_JBE:
        case ZYDIS_MNEMONIC_JNBE:
        case ZYDIS_MNEMONIC_JS:
        case ZYDIS_MNEMONIC_JNS:
        case ZYDIS_MNEMONIC_JP:
        case ZYDIS_MNEMONIC_JNP:
        case ZYDIS_MNEMONIC_JL:
        case ZYDIS_MNEMONIC_JNL:
        case ZYDIS_MNEMONIC_JLE:
        case ZYDIS_MNEMONIC_JNLE:
            if (EvaluateConditionCode(cc)) {
                state.rip = ResolveBranchTarget(ops[0], instr);
            }
            return;
        case ZYDIS_MNEMONIC_SETO:
        case ZYDIS_MNEMONIC_SETNO:
        case ZYDIS_MNEMONIC_SETB:
        case ZYDIS_MNEMONIC_SETNB:
        case ZYDIS_MNEMONIC_SETZ:
        case ZYDIS_MNEMONIC_SETNZ:
        case ZYDIS_MNEMONIC_SETBE:
        case ZYDIS_MNEMONIC_SETNBE:
        case ZYDIS_MNEMONIC_SETS:
        case ZYDIS_MNEMONIC_SETNS:
        case ZYDIS_MNEMONIC_SETP:
        case ZYDIS_MNEMONIC_SETNP:
        case ZYDIS_MNEMONIC_SETL:
        case ZYDIS_MNEMONIC_SETNL:
        case ZYDIS_MNEMONIC_SETLE:
        case ZYDIS_MNEMONIC_SETNLE:
            WriteOperand(ops[0], instr, EvaluateConditionCode(cc) ? 1 : 0);
            return;
        default: // one of the CMOVcc mnemonics
            if (EvaluateConditionCode(cc)) {
                WriteOperand(ops[0], instr, ReadOperand(ops[1], instr));
            }
            return;
        }
    }

    switch (instr.mnemonic) {
    case ZYDIS_MNEMONIC_NOP:
        return;
    case ZYDIS_MNEMONIC_MOV:
    case ZYDIS_MNEMONIC_MOVZX:
        WriteOperand(ops[0], instr, ReadOperand(ops[1], instr));
        return;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD: {
        const int src_width = OperandWidthBytes(ops[1]);
        WriteOperand(ops[0], instr,
                     static_cast<std::uint64_t>(SignExtend(ReadOperand(ops[1], instr), src_width)));
        return;
    }
    case ZYDIS_MNEMONIC_LEA:
        WriteOperand(ops[0], instr, EffectiveAddress(ops[1], instr));
        return;
    case ZYDIS_MNEMONIC_PUSH:
        Push(ReadOperand(ops[0], instr), static_cast<int>(instr.stack_width / 8));
        return;
    case ZYDIS_MNEMONIC_POP:
        WriteOperand(ops[0], instr, Pop(static_cast<int>(instr.stack_width / 8)));
        return;
    case ZYDIS_MNEMONIC_LEAVE:
        state.Reg(Gpr::Rsp) = state.Reg(Gpr::Rbp);
        state.Reg(Gpr::Rbp) = Pop(static_cast<int>(instr.stack_width / 8));
        return;
    case ZYDIS_MNEMONIC_CALL: {
        const std::uint64_t target = ResolveBranchTarget(ops[0], instr);
        Push(state.rip, static_cast<int>(instr.stack_width / 8));
        state.rip = target;
        return;
    }
    case ZYDIS_MNEMONIC_JMP:
        state.rip = ResolveBranchTarget(ops[0], instr);
        return;
    case ZYDIS_MNEMONIC_RET: {
        const std::uint64_t target = Pop(static_cast<int>(instr.stack_width / 8));
        if (instr.operand_count_visible > 0 && ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            state.Reg(Gpr::Rsp) += ReadOperand(ops[0], instr);
        }
        state.rip = target;
        return;
    }
    case ZYDIS_MNEMONIC_ADD:
        WriteOperand(ops[0], instr,
                     UpdateFlagsAdd(ReadOperand(ops[0], instr), ReadOperand(ops[1], instr),
                                    OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_ADC:
        WriteOperand(ops[0], instr,
                     UpdateFlagsAdd(ReadOperand(ops[0], instr), ReadOperand(ops[1], instr),
                                    OperandWidthBytes(ops[0]), state.Flag(Rflags::CF) ? 1 : 0));
        return;
    case ZYDIS_MNEMONIC_SUB:
        WriteOperand(ops[0], instr,
                     UpdateFlagsSub(ReadOperand(ops[0], instr), ReadOperand(ops[1], instr),
                                    OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_SBB:
        WriteOperand(ops[0], instr,
                     UpdateFlagsSub(ReadOperand(ops[0], instr), ReadOperand(ops[1], instr),
                                    OperandWidthBytes(ops[0]), state.Flag(Rflags::CF) ? 1 : 0));
        return;
    case ZYDIS_MNEMONIC_CMP:
        UpdateFlagsSub(ReadOperand(ops[0], instr), ReadOperand(ops[1], instr),
                        OperandWidthBytes(ops[0]));
        return;
    case ZYDIS_MNEMONIC_AND:
        WriteOperand(ops[0], instr,
                     UpdateFlagsLogic(ReadOperand(ops[0], instr) & ReadOperand(ops[1], instr),
                                       OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_OR:
        WriteOperand(ops[0], instr,
                     UpdateFlagsLogic(ReadOperand(ops[0], instr) | ReadOperand(ops[1], instr),
                                       OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_XOR:
        WriteOperand(ops[0], instr,
                     UpdateFlagsLogic(ReadOperand(ops[0], instr) ^ ReadOperand(ops[1], instr),
                                       OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_TEST:
        UpdateFlagsLogic(ReadOperand(ops[0], instr) & ReadOperand(ops[1], instr),
                          OperandWidthBytes(ops[0]));
        return;
    case ZYDIS_MNEMONIC_NOT:
        WriteOperand(ops[0], instr,
                     (~ReadOperand(ops[0], instr)) & MaskFor(OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_NEG:
        WriteOperand(ops[0], instr,
                     UpdateFlagsSub(0, ReadOperand(ops[0], instr), OperandWidthBytes(ops[0])));
        return;
    case ZYDIS_MNEMONIC_INC: {
        const bool saved_cf = state.Flag(Rflags::CF);
        WriteOperand(ops[0], instr,
                     UpdateFlagsAdd(ReadOperand(ops[0], instr), 1, OperandWidthBytes(ops[0])));
        state.SetFlag(Rflags::CF, saved_cf); // INC/DEC never touch CF.
        return;
    }
    case ZYDIS_MNEMONIC_DEC: {
        const bool saved_cf = state.Flag(Rflags::CF);
        WriteOperand(ops[0], instr,
                     UpdateFlagsSub(ReadOperand(ops[0], instr), 1, OperandWidthBytes(ops[0])));
        state.SetFlag(Rflags::CF, saved_cf);
        return;
    }
    case ZYDIS_MNEMONIC_SHL: {
        const int width = OperandWidthBytes(ops[0]);
        const std::uint64_t count =
            (instr.operand_count_visible >= 2 ? ReadOperand(ops[1], instr) : 1) &
            (width == 8 ? 0x3Full : 0x1Full);
        WriteOperand(ops[0], instr, UpdateFlagsShiftLeft(ReadOperand(ops[0], instr), count, width));
        return;
    }
    case ZYDIS_MNEMONIC_SHR: {
        const int width = OperandWidthBytes(ops[0]);
        const std::uint64_t count =
            (instr.operand_count_visible >= 2 ? ReadOperand(ops[1], instr) : 1) &
            (width == 8 ? 0x3Full : 0x1Full);
        WriteOperand(ops[0], instr,
                     UpdateFlagsShiftRight(ReadOperand(ops[0], instr), count, width, false));
        return;
    }
    case ZYDIS_MNEMONIC_SAR: {
        const int width = OperandWidthBytes(ops[0]);
        const std::uint64_t count =
            (instr.operand_count_visible >= 2 ? ReadOperand(ops[1], instr) : 1) &
            (width == 8 ? 0x3Full : 0x1Full);
        WriteOperand(ops[0], instr,
                     UpdateFlagsShiftRight(ReadOperand(ops[0], instr), count, width, true));
        return;
    }
    case ZYDIS_MNEMONIC_CDQE:
        state.Reg(Gpr::Rax) =
            static_cast<std::uint64_t>(SignExtend(state.Reg(Gpr::Rax) & 0xFFFF'FFFFull, 4));
        return;
    case ZYDIS_MNEMONIC_CWDE:
        // A 32-bit GPR write zero-extends into the full 64-bit register (see WriteGpr), so this
        // single assignment correctly leaves the upper 32 bits of RAX at zero.
        state.Reg(Gpr::Rax) =
            static_cast<std::uint64_t>(SignExtend(state.Reg(Gpr::Rax) & 0xFFFFull, 2)) &
            0xFFFF'FFFFull;
        return;
    case ZYDIS_MNEMONIC_CBW:
        state.Reg(Gpr::Rax) = (state.Reg(Gpr::Rax) & ~0xFFFFull) |
                               (static_cast<std::uint64_t>(SignExtend(
                                    state.Reg(Gpr::Rax) & 0xFFull, 1)) &
                                0xFFFFull);
        return;
    case ZYDIS_MNEMONIC_CQO:
        state.Reg(Gpr::Rdx) =
            (static_cast<std::int64_t>(state.Reg(Gpr::Rax)) < 0) ? ~0ull : 0ull;
        return;
    case ZYDIS_MNEMONIC_CDQ:
        state.Reg(Gpr::Rdx) =
            (SignExtend(state.Reg(Gpr::Rax) & 0xFFFF'FFFFull, 4) < 0) ? 0xFFFF'FFFFull : 0ull;
        return;
    case ZYDIS_MNEMONIC_CWD:
        state.Reg(Gpr::Rdx) =
            (state.Reg(Gpr::Rdx) & ~0xFFFFull) |
            ((SignExtend(state.Reg(Gpr::Rax) & 0xFFFFull, 2) < 0) ? 0xFFFFull : 0ull);
        return;
    case ZYDIS_MNEMONIC_MUL: {
        const int width = OperandWidthBytes(ops[0]);
        const std::uint64_t multiplicand = ReadOperand(ops[0], instr) & MaskFor(width);
        if (width == 1) {
            const std::uint64_t product = (state.ReadGpr(ZYDIS_REGISTER_AL) & 0xFF) * multiplicand;
            state.WriteGpr(ZYDIS_REGISTER_AX, product & 0xFFFF);
            const bool overflow = (product & 0xFF00) != 0;
            state.SetFlag(Rflags::CF, overflow);
            state.SetFlag(Rflags::OF, overflow);
        } else {
            const std::uint64_t a = state.ReadGpr(GprByWidth(width, 0));
            const unsigned __int128 product =
                static_cast<unsigned __int128>(a) * multiplicand;
            const std::uint64_t low = static_cast<std::uint64_t>(product) & MaskFor(width);
            const std::uint64_t high =
                static_cast<std::uint64_t>(product >> (width * 8)) & MaskFor(width);
            state.WriteGpr(GprByWidth(width, 0), low);
            state.WriteGpr(GprByWidth(width, 2), high);
            const bool overflow = high != 0;
            state.SetFlag(Rflags::CF, overflow);
            state.SetFlag(Rflags::OF, overflow);
        }
        return;
    }
    case ZYDIS_MNEMONIC_IMUL: {
        const int visible = instr.operand_count_visible;
        if (visible == 1) {
            const int width = OperandWidthBytes(ops[0]);
            if (width == 1) {
                const std::int16_t a =
                    static_cast<std::int8_t>(state.ReadGpr(ZYDIS_REGISTER_AL));
                const std::int16_t b = static_cast<std::int8_t>(ReadOperand(ops[0], instr));
                const std::int16_t product = static_cast<std::int16_t>(a * b);
                state.WriteGpr(ZYDIS_REGISTER_AX, static_cast<std::uint16_t>(product));
                const bool overflow = product != static_cast<std::int8_t>(product);
                state.SetFlag(Rflags::CF, overflow);
                state.SetFlag(Rflags::OF, overflow);
            } else {
                const std::int64_t a = SignExtend(state.ReadGpr(GprByWidth(width, 0)), width);
                const std::int64_t b = SignExtend(ReadOperand(ops[0], instr), width);
                const __int128 full = static_cast<__int128>(a) * b;
                const std::uint64_t low = static_cast<std::uint64_t>(full) & MaskFor(width);
                const std::uint64_t high =
                    static_cast<std::uint64_t>(static_cast<unsigned __int128>(full) >>
                                                (width * 8)) &
                    MaskFor(width);
                state.WriteGpr(GprByWidth(width, 0), low);
                state.WriteGpr(GprByWidth(width, 2), high);
                const bool overflow = full != SignExtend(low, width);
                state.SetFlag(Rflags::CF, overflow);
                state.SetFlag(Rflags::OF, overflow);
            }
        } else {
            const int dst = 0;
            const int lhs_idx = visible == 3 ? 1 : 0;
            const int rhs_idx = visible == 3 ? 2 : 1;
            const int width = OperandWidthBytes(ops[dst]);
            const std::int64_t a = SignExtend(ReadOperand(ops[lhs_idx], instr), width);
            const std::int64_t b = SignExtend(ReadOperand(ops[rhs_idx], instr), width);
            const __int128 full = static_cast<__int128>(a) * b;
            const std::uint64_t truncated = static_cast<std::uint64_t>(full) & MaskFor(width);
            const bool overflow = full != SignExtend(truncated, width);
            state.SetFlag(Rflags::CF, overflow);
            state.SetFlag(Rflags::OF, overflow);
            WriteOperand(ops[dst], instr, truncated);
        }
        return;
    }
    case ZYDIS_MNEMONIC_DIV: {
        const int width = OperandWidthBytes(ops[0]);
        const std::uint64_t divisor = ReadOperand(ops[0], instr) & MaskFor(width);
        if (divisor == 0) {
            Fatal(current_instr_address, "integer division by zero (guest #DE is not "
                                          "emulated yet)");
        }
        if (width == 1) {
            const std::uint64_t ax = state.ReadGpr(ZYDIS_REGISTER_AX);
            const std::uint64_t quotient = ax / divisor;
            if (quotient > 0xFF) {
                Fatal(current_instr_address, "DIV quotient overflow (guest #DE is not "
                                              "emulated yet)");
            }
            state.WriteGpr(ZYDIS_REGISTER_AL, quotient);
            state.WriteGpr(ZYDIS_REGISTER_AH, ax % divisor);
        } else {
            const unsigned __int128 dividend =
                (static_cast<unsigned __int128>(state.ReadGpr(GprByWidth(width, 2)))
                 << (width * 8)) |
                state.ReadGpr(GprByWidth(width, 0));
            const unsigned __int128 quotient = dividend / divisor;
            if (quotient > MaskFor(width)) {
                Fatal(current_instr_address, "DIV quotient overflow (guest #DE is not "
                                              "emulated yet)");
            }
            state.WriteGpr(GprByWidth(width, 0), static_cast<std::uint64_t>(quotient));
            state.WriteGpr(GprByWidth(width, 2),
                           static_cast<std::uint64_t>(dividend % divisor));
        }
        return;
    }
    case ZYDIS_MNEMONIC_IDIV: {
        const int width = OperandWidthBytes(ops[0]);
        const std::int64_t divisor = SignExtend(ReadOperand(ops[0], instr), width);
        if (divisor == 0) {
            Fatal(current_instr_address, "integer division by zero (guest #DE is not "
                                          "emulated yet)");
        }
        if (width == 1) {
            const std::int16_t ax = static_cast<std::int16_t>(state.ReadGpr(ZYDIS_REGISTER_AX));
            const std::int16_t quotient = static_cast<std::int16_t>(ax / divisor);
            if (quotient != static_cast<std::int8_t>(quotient)) {
                Fatal(current_instr_address, "IDIV quotient overflow (guest #DE is not "
                                              "emulated yet)");
            }
            state.WriteGpr(ZYDIS_REGISTER_AL, static_cast<std::uint8_t>(quotient));
            state.WriteGpr(ZYDIS_REGISTER_AH, static_cast<std::uint8_t>(ax % divisor));
        } else {
            const __int128 dividend =
                (static_cast<__int128>(SignExtend(state.ReadGpr(GprByWidth(width, 2)), width))
                 << (width * 8)) |
                state.ReadGpr(GprByWidth(width, 0));
            const __int128 quotient = dividend / divisor;
            if (quotient != SignExtend(static_cast<std::uint64_t>(quotient), width)) {
                Fatal(current_instr_address, "IDIV quotient overflow (guest #DE is not "
                                              "emulated yet)");
            }
            state.WriteGpr(GprByWidth(width, 0), static_cast<std::uint64_t>(quotient));
            state.WriteGpr(GprByWidth(width, 2),
                           static_cast<std::uint64_t>(dividend % divisor));
        }
        return;
    }
    default:
        Unimplemented(instr);
    }
}

} // namespace Core::Interpreter
