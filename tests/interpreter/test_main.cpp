// SPDX-License-Identifier: GPL-2.0-or-later
//
// Tests for the wasm build's x86-64 interpreter (patches/new_files/core/interpreter), run as a
// wasm64 program under Node with the same memory layout as the emulator. The guest functions are
// real assembler output (guest_code.s -> guest_code.inc via gen_guest_code.sh), not hand-encoded
// bytes, so what's being checked is the interpreter's semantics rather than the test's encoding.

#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "core/interpreter/cpu.h"
#include "core/interpreter/guest.h"
#include "core/interpreter/hle.h"
#include "core/paleblood/paleblood.h"
#include "runtime/original.h"

#include "guest_code.inc"

using namespace Core::Interpreter;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
    checks++;
    if (!ok) {
        failures++;
    }
    std::fprintf(stderr, "%s %s\n", ok ? "ok  " : "FAIL", what);
}

// Guest code and data live at fixed guest addresses inside the wasm guest window
// (core/wasm_layout.h); guest addresses are host addresses, so plain pointers work.
constexpr u64 kCode = 0x10000000;
constexpr u64 kData = 0x10100000;

u64 Fn(u64 offset) {
    return kCode + offset;
}

double HostCallback(int a, u64 table, double d) {
    // Called from guest code through an HLE thunk, and itself calls back into guest code.
    const int nested = CallGuest<int>(Fn(k_add), 1, 2);
    return a + d + nested + (table == kData ? 0.0 : 1000.0);
}

// A replacement written the way Paleblood's are (third_party/paleblood/game), against the
// runtime interface this port implements on the interpreter: an original called by name, a
// global, a vtable slot in guest memory and the guest thread pointer. The "executable" is the
// test's code block, mapped as if at the PS4's 0x400000.
constexpr u32 kPs4(u64 offset) {
    return static_cast<u32>(0x400000 + offset);
}
constexpr u64 kPbGlobal = 0x80000; // inside the code block, past the code

struct PbObject;
struct PbVtable {
    void* unknown_slot;
    ::rt::guest_fn<int(PbObject*, int)> scale;
};
struct PbObject {
    const PbVtable* vtable;
    int bias;
};
static_assert(offsetof(PbVtable, scale) == 8 && offsetof(PbObject, bias) == 8);

RT_ORIGINAL(kPs4(k_pb_square), pb_square, int(int));
RT_GLOBAL(kPs4(kPbGlobal), pb_object, PbObject*);

u64 pb_expected_thread_pointer = 0;

extern "C" int pb_replacement(int a, int b) {
    PbObject* object = pb_object.get();
    const bool tp_ok =
        reinterpret_cast<u64>(paleblood_guest_thread_pointer()) == pb_expected_thread_pointer;
    return pb_square(a) + object->vtable->scale(object, b) + (tp_ok ? 0 : 1000);
}

} // namespace

int main() {
    std::memcpy(reinterpret_cast<void*>(kCode), kGuestCode, sizeof(kGuestCode));

    Check(CallGuest<int>(Fn(k_add), 3, 4) == 7, "add: 3 + 4");
    Check(CallGuest<u64>(Fn(k_sum), 100) == 5050, "loop: sum(1..100)");
    Check(CallGuest<u64>(Fn(k_muldiv), 123456789, 1000) == 12345678900ULL, "imul + div");
    Check(CallGuest<s64>(Fn(k_muldiv2), static_cast<u64>(-50), 3) == -21, "imul + cqo + idiv");
    Check(CallGuest<u64>(Fn(k_calls), 10) == 45, "nested call + lea");
    Check(CallGuest<int>(Fn(k_flags), 1, 2) == 1, "cmp/setl (1 < 2)");
    Check(CallGuest<int>(Fn(k_flags), 5, 2) == 100, "cmp/cmovg (5 > 2)");

    auto* data = reinterpret_cast<char*>(kData);
    std::strcpy(data, "interpreter");
    Check(CallGuest<u64>(Fn(k_strlen), data) == 11, "repne scasb strlen");
    CallGuest<u64>(Fn(k_memcpy), data + 64, data, 12);
    Check(std::strcmp(data + 64, "interpreter") == 0, "rep movsb memcpy");

    auto* word = reinterpret_cast<u32*>(kData + 128);
    *word = 5;
    Check(CallGuest<int>(Fn(k_cmpxchg), word, 5, 9) == 1 && *word == 9,
          "lock cmpxchg succeeds when expected matches");
    Check(CallGuest<int>(Fn(k_cmpxchg), word, 5, 1) == 0 && *word == 9,
          "lock cmpxchg fails when expected differs");
    Check(CallGuest<u32>(Fn(k_xadd), word, 3) == 9 && *word == 12, "lock xadd");

    Check(CallGuest<int>(Fn(k_sse_scalar), 6, 7) == 84, "cvtsi2ss/mulss/addss/cvttss2si");

    auto* va = reinterpret_cast<u32*>(kData + 256);
    auto* vb = reinterpret_cast<u32*>(kData + 288);
    for (u32 i = 0; i < 4; i++) {
        va[i] = i + 1;
        vb[i] = (i + 1) * 10;
    }
    const int ptest_zero = CallGuest<int>(Fn(k_sse_packed), va, vb);
    Check(va[0] == 44 && va[1] == 33 && va[2] == 22 && va[3] == 11, "paddd + pshufd 0x1b");
    Check(ptest_zero == 1, "ptest sets ZF for a zero register");

    auto* bytes = reinterpret_cast<u8*>(kData + 320);
    auto* mask = reinterpret_cast<u8*>(kData + 352);
    for (u8 i = 0; i < 16; i++) {
        bytes[i] = i * 3;
        mask[i] = 15 - i;
    }
    mask[0] = 0x80;
    CallGuest<u64>(Fn(k_pshufb), bytes, mask);
    Check(bytes[0] == 0 && bytes[1] == 14 * 3 && bytes[15] == 0, "pshufb (incl. zeroing lane)");

    auto* fa = reinterpret_cast<float*>(kData + 384);
    auto* fb = reinterpret_cast<float*>(kData + 416);
    for (int i = 0; i < 8; i++) {
        fa[i] = 1.5f * i;
        fb[i] = 0.5f;
    }
    const u32 lane4 = CallGuest<u32>(Fn(k_avx), fa, fb);
    float f4;
    std::memcpy(&f4, &lane4, 4);
    Check(fa[7] == 11.0f && f4 == 6.5f, "vaddps ymm + vextractf128 + vmovd");

    Check(CallGuest<int>(Fn(k_x87), 12) == 144, "x87 fild/fmulp/fistp");
    Check(CallGuest<double>(Fn(k_double), 1.5, 2.0) == 12.25, "addsd/mulsd with double args");

    // Guest -> host -> guest: f_hle loads a thunk address from [rdi] and calls it with
    // (int 40, pointer, double 2.5); the host function calls f_add(1, 2) back in the guest.
    *reinterpret_cast<u64*>(kData) = reinterpret_cast<u64>(HleAddress<&HostCallback>());
    Check(CallGuest<double>(Fn(k_hle), kData) == 45.5, "HLE thunk args/return + nested guest call");

    Cpu::Current().State().fs_base = kData + 512;
    *reinterpret_cast<u64*>(kData + 512) = 0x1234567890ULL;
    Check(CallGuest<u64>(Fn(k_tls)) == 0x1234567890ULL, "fs-relative load (TLS)");

    Check(CallGuest<u64>(Fn(k_sum8), 1, 2, 3, 4, 5, 6, 7, 8) == 36,
          "host -> guest call with stack-passed integer arguments");
    Check(CallGuest<double>(Fn(k_dsum9), 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0) == 45.0,
          "host -> guest call with a stack-passed double");

    // Paleblood: before the hook, f_pb_caller(5, 2) runs the original (3*5 + 2) + 5.
    Check(CallGuest<int>(Fn(k_pb_caller), 5, 2) == 22, "paleblood: original before hooking");
    auto* vtable = reinterpret_cast<PbVtable*>(kData + 640);
    auto* object = reinterpret_cast<PbObject*>(kData + 672);
    vtable->scale.address = Fn(k_pb_slot);
    object->vtable = vtable;
    object->bias = 7;
    *reinterpret_cast<PbObject**>(kCode + kPbGlobal) = object;
    pb_expected_thread_pointer = 0x1234567890ULL; // fs:[0], set up for the TLS check above
    const Core::Paleblood::Hook hooks[] = {
        {kPs4(k_pb_original), static_cast<u32>(k_pb_caller - k_pb_original), "pb_replacement",
         [] { return HleThunk<&pb_replacement>::Address(); }},
        {kPs4(k_add), static_cast<u32>(k_sum - k_add), "too short to hook",
         [] { return HleThunk<&pb_replacement>::Address(); }},
    };
    const u32 installed =
        Core::Paleblood::InstallHooks(reinterpret_cast<u8*>(kCode), 0x100000, hooks);
    Check(installed == 1 && CallGuest<int>(Fn(k_add), 3, 4) == 7,
          "paleblood: a function shorter than the jump isn't hooked");
    // Now the replacement runs instead: 5*5 + 2*7 (vtable slot) + 5.
    Check(CallGuest<int>(Fn(k_pb_caller), 5, 2) == 44,
          "paleblood: replacement runs, calls an original, a global and a vtable slot");

    bool faulted = false;
    try {
        CallGuest<u64>(Fn(k_strlen), static_cast<u64>(8)); // reads from address 8
    } catch (const GuestFault&) {
        faulted = true;
    }
    Check(faulted, "wild guest pointer raises GuestFault instead of corrupting memory");

    // Throughput, for the record (not a pass/fail check): the sum loop is 5 instructions per
    // iteration.
    const u64 before = Cpu::Current().InstructionsExecuted();
    const auto t0 = std::chrono::steady_clock::now();
    const u64 sum = CallGuest<u64>(Fn(k_sum), 20'000'000);
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const u64 executed = Cpu::Current().InstructionsExecuted() - before;
    std::fprintf(stderr, "throughput: %llu guest instructions in %.2fs = %.1f MIPS (sum %llu)\n",
                 static_cast<unsigned long long>(executed), secs, executed / secs / 1e6,
                 static_cast<unsigned long long>(sum));

    std::fprintf(stderr, "%d/%d checks passed\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
