// SPDX-License-Identifier: GPL-2.0-or-later
//
// Hand-assembled x86-64 machine code exercising Core::Interpreter, run with a plain native
// compiler (no Emscripten, no shadPS4 build required - see CMakeLists.txt in this directory).
// Every byte sequence below is commented with the instruction it encodes; this is deliberately
// not using an external assembler so there is no second tool whose own correctness this would
// need to trust.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "core/interpreter/guest_memory.h"
#include "core/interpreter/host_thunk.h"
#include "core/interpreter/x86_interpreter.h"

using namespace Core::Interpreter;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    } else {
        std::fprintf(stderr, "ok:   %s\n", what);
    }
}

// A flat buffer standing in for guest memory: VAddr is just an offset into it. Nowhere near
// what the real integration needs (see PROJECT.md, "Guest virtual memory") - this exists only
// to exercise the interpreter's instruction semantics and call-boundary mechanics in isolation.
class FlatGuestMemory final : public GuestMemory {
public:
    explicit FlatGuestMemory(std::size_t size) : bytes(size, 0) {}

    std::uint8_t* Translate(VAddr vaddr, std::size_t size, bool /*for_write*/) override {
        if (vaddr + size > bytes.size()) {
            std::fprintf(stderr, "FlatGuestMemory: out-of-range access at %#llx (+%zu)\n",
                          static_cast<unsigned long long>(vaddr), size);
            std::abort();
        }
        return bytes.data() + vaddr;
    }

    void LoadCode(VAddr at, std::initializer_list<std::uint8_t> code) {
        std::size_t i = 0;
        for (std::uint8_t b : code) {
            bytes[at + i] = b;
            ++i;
        }
    }

private:
    std::vector<std::uint8_t> bytes;
};

// --- Test A: SysV calling convention + basic ALU --------------------------------------------
// mov eax, edi ; add eax, esi ; ret        -- computes edi + esi
void TestAddFunction() {
    FlatGuestMemory mem(0x10000);
    HostThunkTable thunks;
    X86Interpreter cpu(mem, thunks);
    constexpr VAddr kCode = 0x1000;
    mem.LoadCode(kCode,
                 {0x89, 0xF8,             // mov eax, edi
                  0x01, 0xF0,             // add eax, esi
                  0xC3});                 // ret
    cpu.State().Reg(Gpr::Rsp) = 0x8000;
    const std::uint64_t result = cpu.CallGuestFunction(kCode, {3, 4});
    Check(result == 7, "TestAddFunction: 3 + 4 == 7");
}

// --- Test B: control flow (loop with a conditional branch) ------------------------------------
// Computes the triangular number sum(1..n) for n = rdi, using a counting loop with CMP/JGE/
// INC/ADD/JMP. See the design notes for the byte-by-byte derivation; summarized inline below.
void TestLoopTriangularNumber() {
    FlatGuestMemory mem(0x10000);
    HostThunkTable thunks;
    X86Interpreter cpu(mem, thunks);
    constexpr VAddr kCode = 0x1000;
    mem.LoadCode(kCode,
                 {0x31, 0xC0,             // xor eax, eax        ; eax = 0 (accumulator)
                  0x31, 0xC9,             // xor ecx, ecx        ; ecx = 0 (counter i)
                  0x39, 0xF9,             // cmp ecx, edi        ; loop: i vs n
                  0x7D, 0x06,             // jge +6              ; if i >= n, done (-> offset 14)
                  0xFF, 0xC1,             // inc ecx             ; i++
                  0x01, 0xC8,             // add eax, ecx        ; sum += i
                  0xEB, 0xF6,             // jmp -10             ; back to the cmp at offset 4
                  0xC3});                 // ret                 ; done (offset 14)
    cpu.State().Reg(Gpr::Rsp) = 0x8000;
    const std::uint64_t result = cpu.CallGuestFunction(kCode, {5});
    Check(result == 15, "TestLoopTriangularNumber: sum(1..5) == 15");
}

// --- Test C: flags + SETcc + MOVZX --------------------------------------------------------------
// cmp edi, esi ; sete al ; movzx eax, al ; ret   -- returns 1 if edi == esi, else 0
void TestCompareEqual() {
    FlatGuestMemory mem(0x10000);
    HostThunkTable thunks;
    X86Interpreter cpu(mem, thunks);
    constexpr VAddr kCode = 0x1000;
    mem.LoadCode(kCode,
                 {0x39, 0xF7,             // cmp edi, esi
                  0x0F, 0x94, 0xC0,       // sete al
                  0x0F, 0xB6, 0xC0,       // movzx eax, al
                  0xC3});                 // ret
    cpu.State().Reg(Gpr::Rsp) = 0x8000;
    Check(cpu.CallGuestFunction(kCode, {9, 9}) == 1, "TestCompareEqual: 9 == 9 -> 1");
    Check(cpu.CallGuestFunction(kCode, {9, 8}) == 0, "TestCompareEqual: 9 != 8 -> 0");
}

// --- Test D: the guest<->host call boundary (HostThunkTable) ------------------------------------
// movabs rax, <thunk address> ; call rax ; add eax, 1 ; ret
// The thunk itself is a real host C++ function that doubles rdi - standing in for what a PS4
// HLE library import (core/libraries/**) will eventually be once this is wired into shadPS4 for
// real (see PROJECT.md, "Guest<->host call boundary").
void TestHostThunkBoundary() {
    FlatGuestMemory mem(0x10000);
    HostThunkTable thunks;
    const std::uint64_t thunk_addr = thunks.Register([](X86Interpreter& cpu) {
        cpu.State().Reg(Gpr::Rax) = cpu.State().Reg(Gpr::Rdi) * 2;
    });

    X86Interpreter cpu(mem, thunks);
    constexpr VAddr kCode = 0x1000;
    std::vector<std::uint8_t> code = {0x48, 0xB8}; // movabs rax, imm64
    for (int i = 0; i < 8; i++) {
        code.push_back(static_cast<std::uint8_t>(thunk_addr >> (8 * i)));
    }
    code.push_back(0xFF);
    code.push_back(0xD0); // call rax
    code.push_back(0x05); // add eax, imm32
    code.push_back(0x01);
    code.push_back(0x00);
    code.push_back(0x00);
    code.push_back(0x00);
    code.push_back(0xC3); // ret
    mem.LoadCode(kCode, {});
    for (std::size_t i = 0; i < code.size(); i++) {
        mem.Translate(kCode + i, 1, true)[0] = code[i];
    }
    cpu.State().Reg(Gpr::Rsp) = 0x8000;
    const std::uint64_t result = cpu.CallGuestFunction(kCode, {20});
    Check(result == 41, "TestHostThunkBoundary: thunk(20)*2 + 1 == 41");
}

// --- Test E: RunMainEntry's exact stack/register setup -----------------------------------------
// Mirrors what Core::Linker::RunMainEntry's inline asm does: verifies the guest sees the same
// 16 bytes both through rdi (the params pointer) and copied onto the stack, in the documented
// field order.
struct ExitSignal {
    std::uint64_t rax, rbx, rcx, rdx;
};

void TestRunMainEntryStackLayout() {
    FlatGuestMemory mem(0x10000);
    HostThunkTable thunks;
    const std::uint64_t exit_addr = thunks.Register([](X86Interpreter& cpu) {
        throw ExitSignal{cpu.State().Reg(Gpr::Rax), cpu.State().Reg(Gpr::Rbx),
                          cpu.State().Reg(Gpr::Rcx), cpu.State().Reg(Gpr::Rdx)};
    });

    X86Interpreter cpu(mem, thunks);
    constexpr VAddr kCode = 0x1000;
    constexpr VAddr kParams = 0x2000;
    mem.Translate(kParams, 8, true);
    const std::uint64_t field0 = 0x1111'1111'2222'2222ull;
    const std::uint64_t field1 = 0x3333'3333'4444'4444ull;
    mem.Write(kParams, field0, 8);
    mem.Write(kParams + 8, field1, 8);

    mem.LoadCode(kCode,
                 {0x48, 0x8B, 0x07,             // mov rax, [rdi]
                  0x48, 0x8B, 0x5F, 0x08,       // mov rbx, [rdi+8]
                  0x48, 0x8B, 0x0C, 0x24,       // mov rcx, [rsp]
                  0x48, 0x8B, 0x54, 0x24, 0x08, // mov rdx, [rsp+8]
                  0xFF, 0xD6,                   // call rsi
                  0xC3});                       // ret (unreachable: the thunk throws)

    cpu.State().Reg(Gpr::Rsp) = 0x8000;
    bool threw = false;
    try {
        cpu.RunMainEntry(kCode, kParams, exit_addr);
    } catch (const ExitSignal& signal) {
        threw = true;
        Check(signal.rax == field0, "TestRunMainEntryStackLayout: [rdi] == field0");
        Check(signal.rbx == field1, "TestRunMainEntryStackLayout: [rdi+8] == field1");
        Check(signal.rcx == field0, "TestRunMainEntryStackLayout: [rsp] == field0 (stack copy)");
        Check(signal.rdx == field1, "TestRunMainEntryStackLayout: [rsp+8] == field1 (stack copy)");
    }
    Check(threw, "TestRunMainEntryStackLayout: exit thunk was reached");
}

} // namespace

int main() {
    TestAddFunction();
    TestLoopTriangularNumber();
    TestCompareEqual();
    TestHostThunkBoundary();
    TestRunMainEntryStackLayout();

    if (g_failures > 0) {
        std::fprintf(stderr, "\n%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::fprintf(stderr, "\nAll checks passed\n");
    return 0;
}
