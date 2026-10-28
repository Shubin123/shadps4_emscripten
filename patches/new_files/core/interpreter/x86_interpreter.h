// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A from-scratch x86-64 instruction interpreter, written for the Emscripten/WebAssembly build
// where shadPS4's normal execution model - running the guest PS4 ELF's x86-64 code directly on
// the host CPU (see Core::Linker::RunMainEntry, core/cpu_patches.cpp, core/signals.cpp) - is not
// possible: WebAssembly cannot be handed arbitrary machine code to execute, and has nothing like
// the POSIX SIGSEGV/SIGILL handling that model depends on. See PROJECT.md for the full picture,
// including the two integration pieces this interpreter does not yet solve on its own:
//  - Guest virtual memory: guest VAs can't be identity-mapped to wasm linear memory the way
//    Core::AddressSpace maps them to real host virtual memory, so GuestMemory is an explicit
//    translation seam rather than a raw pointer.
//  - The guest<->host call boundary: anywhere existing shadPS4 code resolves a guest import or
//    callback to a native C++ function pointer and calls it directly needs to route through
//    HostThunkTable instead once this is wired into the real codebase.
//
// Instruction coverage is intentionally scoped to integer/control-flow code for this first pass:
// general-purpose arithmetic/logic, shifts, jumps/calls/returns, and the condition-code
// instruction families (Jcc/SETcc/CMOVcc). SSE/AVX floating point and REP-prefixed string
// instructions are not implemented - Execute() calls Unimplemented() for them, which aborts with
// a diagnostic rather than silently executing the wrong thing. See PROJECT.md for why those were
// deferred (in short: real ambiguity in disassembler mnemonic naming that deserves its own
// careful pass, not a guess).

#pragma once

#include <cstdint>
#include <initializer_list>
#include <Zydis/Zydis.h>
#include "core/interpreter/guest_memory.h"
#include "core/interpreter/host_thunk.h"
#include "core/interpreter/x86_register_file.h"

namespace Core::Interpreter {

// No real guest call can produce this as a return address: shadPS4's guest user address space is
// 40 bits wide (Core::AddressSpace::Traits::ADDRESS_SPACE_BITS), and this sits above even the
// host-thunk range (see host_thunk.h). CallGuestFunction uses it to recognize "the call returned"
// without a separate call-depth counter, mirroring how real position-independent return-address
// sentinels work in other user-mode CPU emulators.
inline constexpr std::uint64_t kCallSentinel = 0xFFFE'0000'0000'0000ull;

class X86Interpreter {
public:
    explicit X86Interpreter(GuestMemory& memory, HostThunkTable& thunks);

    CpuState& State() {
        return state;
    }
    const CpuState& State() const {
        return state;
    }

    // Executes guest instructions starting at state.rip until rip == stop_address. If rip lands
    // on an address registered in the thunk table at any point (including stop_address itself,
    // which is how CallGuestFunction's sentinel is recognized when a tail call reaches it), the
    // thunk is invoked natively, its result becomes RAX, and the simulated `ret` that follows is
    // performed before control returns to this loop.
    void RunUntil(std::uint64_t stop_address);

    // Calls a guest function using the System V AMD64 calling convention: up to 6 integer/pointer
    // arguments in rdi, rsi, rdx, rcx, r8, r9. More than 6 arguments, and XMM/float arguments,
    // are not implemented (ASSERT below) since nothing exercises them yet. Returns RAX.
    std::uint64_t CallGuestFunction(std::uint64_t function_address,
                                     std::initializer_list<std::uint64_t> args = {});

    // Mirrors Core::Linker::RunMainEntry's inline-asm trampoline (src/core/linker.cpp) exactly:
    // copies 16 bytes starting at *params_address onto the guest stack in the same
    // high-then-low field order the original pushq/pushq pair does, aligns/offsets rsp the same
    // way, sets rdi = params_address, and jumps to entry_address. `exit_address` takes the place
    // of ProgramExitFunc's raw host function pointer: it should be a HostThunkTable address
    // (there is nothing else it safely could be once execution is interpreted - a real host
    // pointer is not valid guest-decodable bytes). Never returns.
    [[noreturn]] void RunMainEntry(std::uint64_t entry_address, std::uint64_t params_address,
                                    std::uint64_t exit_address);

private:
    void Step();
    void Execute(const ZydisDecodedInstruction& instr, const ZydisDecodedOperand* ops);
    [[noreturn]] void Unimplemented(const ZydisDecodedInstruction& instr) const;

    // These all address relative to `current_instr_address` (the address Step() fetched the
    // instruction from, set before Execute() runs - needed because RIP-relative memory operands
    // are defined relative to the start of their own instruction, not wherever rip has been
    // advanced to for a pushed return address by the time a handler runs).
    std::uint64_t EffectiveAddress(const ZydisDecodedOperand& op,
                                    const ZydisDecodedInstruction& instr) const;
    // Resolves a CALL/JMP/Jcc target operand to an absolute address: for a register operand,
    // that is the register's value directly (not an address to dereference); for a relative
    // immediate, current_instr_address + instr.length + the signed displacement; for a memory
    // operand (`call [rax+8]` etc.), EffectiveAddress() followed by a dereference.
    std::uint64_t ResolveBranchTarget(const ZydisDecodedOperand& op,
                                       const ZydisDecodedInstruction& instr);
    std::uint64_t ReadOperand(const ZydisDecodedOperand& op, const ZydisDecodedInstruction& instr);
    void WriteOperand(const ZydisDecodedOperand& op, const ZydisDecodedInstruction& instr,
                       std::uint64_t value);
    int OperandWidthBytes(const ZydisDecodedOperand& op) const;

    void Push(std::uint64_t value, int width);
    std::uint64_t Pop(int width);

    // `width` is the operand width in bytes (1, 2, 4 or 8). Each masks lhs/rhs to `width` first
    // (safe: for add/sub, the low `width` bytes of the result only ever depend on the low
    // `width` bytes of the inputs) and returns the masked result so callers can write it
    // straight back to the destination operand. `carry_in`/`borrow_in` (0 or 1) let ADC/SBB
    // reuse the same logic as ADD/SUB.
    std::uint64_t UpdateFlagsAdd(std::uint64_t lhs, std::uint64_t rhs, int width,
                                  std::uint64_t carry_in = 0);
    std::uint64_t UpdateFlagsSub(std::uint64_t lhs, std::uint64_t rhs, int width,
                                  std::uint64_t borrow_in = 0);
    std::uint64_t UpdateFlagsLogic(std::uint64_t result, int width);
    // Shift flag updates need the pre-shift value (for CF, the last bit shifted out) and the
    // shift count (x86 leaves flags untouched entirely when count == 0, and only defines OF for
    // count == 1). Returns the masked post-shift result.
    std::uint64_t UpdateFlagsShiftLeft(std::uint64_t original, std::uint64_t count, int width);
    std::uint64_t UpdateFlagsShiftRight(std::uint64_t original, std::uint64_t count, int width,
                                         bool arithmetic);
    void SetSzp(std::uint64_t result, int width);

    // Index 0-15 using the standard x86 condition-code nibble order (0=O,1=NO,2=B,3=NB,4=Z,5=NZ,
    // 6=BE,7=NBE,8=S,9=NS,10=P,11=NP,12=L,13=NL,14=LE,15=NLE); matches the order Intel assigns
    // opcodes for Jcc/SETcc/CMOVcc, which is why mapping a mnemonic to one is a plain lookup.
    bool EvaluateConditionCode(int cc) const;
    int ConditionCodeOf(ZydisMnemonic mnemonic) const;

    GuestMemory& memory;
    HostThunkTable& thunks;
    ZydisDecoder decoder;
    CpuState state;
    std::uint64_t current_instr_address = 0;
};

} // namespace Core::Interpreter
