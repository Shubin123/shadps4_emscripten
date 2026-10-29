// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// General-purpose integer, control-flow, string and system instructions.

#include <bit>
#include <mutex>
#include "common/rdtsc.h"
#include "core/interpreter/cpu.h"

namespace Core::Interpreter {

namespace {

constexpr u64 Mask(u8 size) {
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}
constexpr u64 SignBit(u8 size) {
    return 1ULL << (size * 8 - 1);
}
constexpr s64 SignExtend(u64 v, u8 size) {
    switch (size) {
    case 1:
        return static_cast<s8>(v);
    case 2:
        return static_cast<s16>(v);
    case 4:
        return static_cast<s32>(v);
    default:
        return static_cast<s64>(v);
    }
}

// Unaligned locked accesses can't use wasm atomics (which trap when misaligned), so they
// serialize on this instead. Real x86 code almost never does a split lock.
std::mutex g_split_lock;

template <typename T, typename F>
T AtomicRmw(u8* p, F&& f) {
    T* tp = reinterpret_cast<T*>(p);
    if (reinterpret_cast<uintptr_t>(p) % sizeof(T) != 0) {
        std::scoped_lock lk{g_split_lock};
        T old;
        std::memcpy(&old, p, sizeof(T));
        const T nv = static_cast<T>(f(old));
        std::memcpy(p, &nv, sizeof(T));
        return old;
    }
    T old = __atomic_load_n(tp, __ATOMIC_SEQ_CST);
    while (!__atomic_compare_exchange_n(tp, &old, static_cast<T>(f(old)), false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
    }
    return old;
}

// Read-modify-write of an operand: atomic when it's memory and the instruction is LOCKed (or is
// XCHG, which locks implicitly). Returns the old value; `f` maps old -> new.
template <typename F>
u64 Rmw(Cpu& cpu, const Insn& in, const Op& o, bool locked, F&& f) {
    if (o.kind != OpKind::Mem || !locked) {
        const u64 old = cpu.Read(o);
        cpu.Write(o, f(old));
        return old;
    }
    u8* p = cpu.Ptr(cpu.EffectiveAddress(o), o.size);
    switch (o.size) {
    case 1:
        return AtomicRmw<u8>(p, f);
    case 2:
        return AtomicRmw<u16>(p, f);
    case 4:
        return AtomicRmw<u32>(p, f);
    default:
        return AtomicRmw<u64>(p, f);
    }
}

int CcOf(u16 m, u16 first) {
    return static_cast<int>(m - first);
}

void Cpuid(CpuState& s) {
    const u32 leaf = static_cast<u32>(s.gpr[RAX]);
    const u32 sub = static_cast<u32>(s.gpr[RCX]);
    u32 a = 0, b = 0, c = 0, d = 0;
    // The PS4's CPU: AMD Jaguar (family 16h), 8 cores.
    switch (leaf) {
    case 0:
        a = 0xD;
        b = 0x68747541; // "Auth"
        d = 0x69746E65; // "enti"
        c = 0x444D4163; // "cAMD"
        break;
    case 1:
        a = 0x00700F01; // family 16h model 30h
        b = (8u << 16) | (1u << 11);
        // SSE3 PCLMUL SSSE3 CX16 SSE4.1 SSE4.2 MOVBE POPCNT AES XSAVE OSXSAVE AVX F16C
        c = (1u << 0) | (1u << 1) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) |
            (1u << 22) | (1u << 23) | (1u << 25) | (1u << 26) | (1u << 27) | (1u << 28) |
            (1u << 29);
        // FPU TSC CX8 CMOV MMX FXSR SSE SSE2 HTT
        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 24) |
            (1u << 25) | (1u << 26) | (1u << 28);
        break;
    case 7:
        if (sub == 0) {
            b = (1u << 3); // BMI1
        }
        break;
    case 0xD:
        if (sub == 0) {
            a = 7; // x87 | SSE | AVX state
            b = c = 0x340;
        } else if (sub == 1) {
            a = 1; // XSAVEOPT
        }
        break;
    case 0x80000000:
        a = 0x8000001E;
        break;
    case 0x80000001:
        c = (1u << 0) | (1u << 5) | (1u << 6) | (1u << 8); // LAHF64 LZCNT SSE4A PREFETCHW
        d = (1u << 11) | (1u << 20) | (1u << 27) | (1u << 29); // SYSCALL NX RDTSCP LM
        break;
    default:
        break;
    }
    s.gpr[RAX] = a;
    s.gpr[RBX] = b;
    s.gpr[RCX] = c;
    s.gpr[RDX] = d;
}

} // namespace

bool Cpu::ExecInteger(const Insn& in) {
    const Op& o0 = in.op[0];
    const Op& o1 = in.op[1];
    const u8 sz = o0.size ? o0.size : in.opsize;
    const bool locked = in.attrs & kLock;

    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_NOP:
    case ZYDIS_MNEMONIC_PAUSE:
    case ZYDIS_MNEMONIC_ENDBR64:
    case ZYDIS_MNEMONIC_ENDBR32:
    case ZYDIS_MNEMONIC_LFENCE:
    case ZYDIS_MNEMONIC_SFENCE:
    case ZYDIS_MNEMONIC_PREFETCHT0:
    case ZYDIS_MNEMONIC_PREFETCHT1:
    case ZYDIS_MNEMONIC_PREFETCHT2:
    case ZYDIS_MNEMONIC_PREFETCHNTA:
    case ZYDIS_MNEMONIC_PREFETCH:
    case ZYDIS_MNEMONIC_PREFETCHW:
    case ZYDIS_MNEMONIC_CLFLUSH:
    case ZYDIS_MNEMONIC_CLFLUSHOPT:
        return true;
    case ZYDIS_MNEMONIC_MFENCE:
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return true;

    // ---- data movement ----
    case ZYDIS_MNEMONIC_MOV:
        if (o0.kind == OpKind::Reg && o0.reg >= kRegVec) {
            return false;
        }
        Write(o0, Read(o1));
        return true;
    case ZYDIS_MNEMONIC_MOVZX:
        Write(o0, Read(o1));
        return true;
    case ZYDIS_MNEMONIC_MOVSX:
    case ZYDIS_MNEMONIC_MOVSXD:
        Write(o0, static_cast<u64>(SignExtend(Read(o1), o1.size)));
        return true;
    case ZYDIS_MNEMONIC_LEA:
        Write(o0, EffectiveAddress(o1) & ((in.attrs & kAddr32) ? 0xFFFFFFFFULL : ~0ULL));
        return true;
    case ZYDIS_MNEMONIC_XCHG: {
        if (o0.kind == OpKind::Mem || o1.kind == OpKind::Mem) {
            const Op& m = o0.kind == OpKind::Mem ? o0 : o1;
            const Op& r = o0.kind == OpKind::Mem ? o1 : o0;
            const u64 rv = Read(r);
            const u64 old = Rmw(*this, in, m, true, [&](u64) { return rv; });
            Write(r, old);
        } else {
            const u64 a = Read(o0), b = Read(o1);
            Write(o0, b);
            Write(o1, a);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_BSWAP: {
        const u64 v = Read(o0);
        Write(o0, sz == 8 ? std::byteswap(v) : std::byteswap(static_cast<u32>(v)));
        return true;
    }
    case ZYDIS_MNEMONIC_MOVBE: {
        const u64 v = Read(o1);
        u64 r = sz == 8 ? std::byteswap(v)
                : sz == 4 ? std::byteswap(static_cast<u32>(v))
                          : std::byteswap(static_cast<u16>(v));
        Write(o0, r);
        return true;
    }
    case ZYDIS_MNEMONIC_CMPXCHG: {
        const u64 expected = ReadGpr(RAX, sz);
        const u64 src = Read(o1);
        u64 old;
        if (o0.kind == OpKind::Mem && o0.size >= 1 &&
            reinterpret_cast<uintptr_t>(Ptr(EffectiveAddress(o0), sz)) % sz == 0) {
            u8* p = Ptr(EffectiveAddress(o0), sz);
            switch (sz) {
            case 1: {
                u8 e = static_cast<u8>(expected);
                __atomic_compare_exchange_n(p, &e, static_cast<u8>(src), false, __ATOMIC_SEQ_CST,
                                            __ATOMIC_SEQ_CST);
                old = e;
                break;
            }
            case 2: {
                u16 e = static_cast<u16>(expected);
                __atomic_compare_exchange_n(reinterpret_cast<u16*>(p), &e, static_cast<u16>(src),
                                            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                old = e;
                break;
            }
            case 4: {
                u32 e = static_cast<u32>(expected);
                __atomic_compare_exchange_n(reinterpret_cast<u32*>(p), &e, static_cast<u32>(src),
                                            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                old = e;
                break;
            }
            default: {
                u64 e = expected;
                __atomic_compare_exchange_n(reinterpret_cast<u64*>(p), &e, src, false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
                old = e;
                break;
            }
            }
        } else {
            std::scoped_lock lk{g_split_lock};
            old = Read(o0);
            if (old == expected) {
                Write(o0, src);
            } else if (o0.kind == OpKind::Reg) {
                Write(o0, old);
            }
        }
        FlagsSub(expected, old, 0, sz);
        if (old != expected) {
            WriteGpr(RAX, sz, old);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_CMPXCHG8B:
    case ZYDIS_MNEMONIC_CMPXCHG16B: {
        const bool wide = in.mnemonic == ZYDIS_MNEMONIC_CMPXCHG16B;
        const u64 addr = EffectiveAddress(o0);
        std::scoped_lock lk{g_split_lock};
        if (wide) {
            u64 cur[2];
            std::memcpy(cur, Ptr(addr, 16), 16);
            if (cur[0] == s.gpr[RAX] && cur[1] == s.gpr[RDX]) {
                const u64 nv[2] = {s.gpr[RBX], s.gpr[RCX]};
                std::memcpy(Ptr(addr, 16), nv, 16);
                s.rflags |= ZF;
            } else {
                s.gpr[RAX] = cur[0];
                s.gpr[RDX] = cur[1];
                s.rflags &= ~ZF;
            }
        } else {
            const u64 cur = Load<u64>(addr);
            const u64 exp = (s.gpr[RDX] << 32) | (s.gpr[RAX] & 0xFFFFFFFF);
            if (cur == exp) {
                Store<u64>(addr, (s.gpr[RCX] << 32) | (s.gpr[RBX] & 0xFFFFFFFF));
                s.rflags |= ZF;
            } else {
                s.gpr[RAX] = cur & 0xFFFFFFFF;
                s.gpr[RDX] = cur >> 32;
                s.rflags &= ~ZF;
            }
        }
        return true;
    }
    case ZYDIS_MNEMONIC_XADD: {
        const u64 src = Read(o1);
        const u64 old = Rmw(*this, in, o0, locked, [&](u64 v) { return v + src; });
        FlagsAdd(old, src, 0, sz);
        Write(o1, old);
        return true;
    }
    case ZYDIS_MNEMONIC_PUSH: {
        u64 v = Read(o0);
        if (o0.kind == OpKind::Imm) {
            v = static_cast<u64>(o0.value);
        }
        if (in.opsize == 2) {
            s.gpr[RSP] -= 2;
            Store<u16>(s.gpr[RSP], static_cast<u16>(v));
        } else {
            Push64(v);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_POP: {
        if (in.opsize == 2) {
            const u16 v = Load<u16>(s.gpr[RSP]);
            s.gpr[RSP] += 2;
            Write(o0, v);
        } else {
            const u64 v = Pop64();
            Write(o0, v);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_PUSHFQ:
    case ZYDIS_MNEMONIC_PUSHF:
        Push64(s.rflags | 0x2);
        return true;
    case ZYDIS_MNEMONIC_POPFQ:
    case ZYDIS_MNEMONIC_POPF: {
        const u64 v = Pop64();
        s.rflags = (v & (kStatusFlags | DF | TF | (1ULL << 21))) | 0x202;
        return true;
    }
    case ZYDIS_MNEMONIC_LEAVE:
        s.gpr[RSP] = s.gpr[RBP];
        s.gpr[RBP] = Pop64();
        return true;
    case ZYDIS_MNEMONIC_ENTER: {
        const u64 frame = static_cast<u64>(o0.value);
        Push64(s.gpr[RBP]);
        const u64 fp = s.gpr[RSP];
        const u8 level = static_cast<u8>(o1.value) & 31;
        for (u8 i = 1; i < level; i++) {
            s.gpr[RBP] -= 8;
            Push64(Load<u64>(s.gpr[RBP]));
        }
        if (level > 0) {
            Push64(fp);
        }
        s.gpr[RBP] = fp;
        s.gpr[RSP] -= frame;
        return true;
    }
    case ZYDIS_MNEMONIC_CBW:
        WriteGpr(RAX, 2, static_cast<u64>(static_cast<s8>(s.gpr[RAX])));
        return true;
    case ZYDIS_MNEMONIC_CWDE:
        WriteGpr(RAX, 4, static_cast<u64>(static_cast<s16>(s.gpr[RAX])));
        return true;
    case ZYDIS_MNEMONIC_CDQE:
        s.gpr[RAX] = static_cast<u64>(static_cast<s32>(s.gpr[RAX]));
        return true;
    case ZYDIS_MNEMONIC_CWD:
        WriteGpr(RDX, 2, (s.gpr[RAX] & 0x8000) ? 0xFFFF : 0);
        return true;
    case ZYDIS_MNEMONIC_CDQ:
        WriteGpr(RDX, 4, (s.gpr[RAX] & 0x80000000) ? 0xFFFFFFFF : 0);
        return true;
    case ZYDIS_MNEMONIC_CQO:
        s.gpr[RDX] = (s.gpr[RAX] >> 63) ? ~0ULL : 0;
        return true;

    // ---- arithmetic ----
    case ZYDIS_MNEMONIC_ADD:
    case ZYDIS_MNEMONIC_ADC: {
        const u64 b = Read(o1);
        const u64 c = in.mnemonic == ZYDIS_MNEMONIC_ADC ? (s.rflags & CF) : 0;
        const u64 old = Rmw(*this, in, o0, locked, [&](u64 a) { return a + b + c; });
        FlagsAdd(old, b, c, sz);
        return true;
    }
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_SBB: {
        const u64 b = Read(o1);
        const u64 c = in.mnemonic == ZYDIS_MNEMONIC_SBB ? (s.rflags & CF) : 0;
        const u64 old = Rmw(*this, in, o0, locked, [&](u64 a) { return a - b - c; });
        FlagsSub(old, b, c, sz);
        return true;
    }
    case ZYDIS_MNEMONIC_CMP:
        FlagsSub(Read(o0), Read(o1), 0, sz);
        return true;
    case ZYDIS_MNEMONIC_AND:
    case ZYDIS_MNEMONIC_OR:
    case ZYDIS_MNEMONIC_XOR: {
        const u64 b = Read(o1);
        const u16 m = in.mnemonic;
        const auto op = [&](u64 a) {
            return m == ZYDIS_MNEMONIC_AND ? (a & b) : m == ZYDIS_MNEMONIC_OR ? (a | b) : (a ^ b);
        };
        const u64 old = Rmw(*this, in, o0, locked, op);
        FlagsLogic(op(old), sz);
        return true;
    }
    case ZYDIS_MNEMONIC_TEST:
        FlagsLogic(Read(o0) & Read(o1), sz);
        return true;
    case ZYDIS_MNEMONIC_NOT:
        Rmw(*this, in, o0, locked, [](u64 a) { return ~a; });
        return true;
    case ZYDIS_MNEMONIC_NEG: {
        const u64 old = Rmw(*this, in, o0, locked, [](u64 a) { return 0 - a; });
        FlagsSub(0, old, 0, sz);
        if ((old & Mask(sz)) == 0) {
            s.rflags &= ~CF;
        } else {
            s.rflags |= CF;
        }
        return true;
    }
    case ZYDIS_MNEMONIC_INC:
    case ZYDIS_MNEMONIC_DEC: {
        const bool inc = in.mnemonic == ZYDIS_MNEMONIC_INC;
        const u64 cf = s.rflags & CF;
        const u64 old = Rmw(*this, in, o0, locked, [&](u64 a) { return inc ? a + 1 : a - 1; });
        if (inc) {
            FlagsAdd(old, 1, 0, sz);
        } else {
            FlagsSub(old, 1, 0, sz);
        }
        s.rflags = (s.rflags & ~CF) | cf; // INC/DEC leave CF alone
        return true;
    }
    case ZYDIS_MNEMONIC_MUL: {
        const u64 a = ReadGpr(RAX, sz), b = Read(o0);
        bool hi_nonzero;
        if (sz == 8) {
            const unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
            s.gpr[RAX] = static_cast<u64>(r);
            s.gpr[RDX] = static_cast<u64>(r >> 64);
            hi_nonzero = s.gpr[RDX] != 0;
        } else if (sz == 1) {
            const u64 r = a * b;
            WriteGpr(RAX, 2, r);
            hi_nonzero = (r >> 8) != 0;
        } else {
            const u64 r = a * b;
            WriteGpr(RAX, sz, r);
            WriteGpr(RDX, sz, r >> (sz * 8));
            hi_nonzero = (r >> (sz * 8)) != 0;
        }
        s.rflags = (s.rflags & ~(CF | OF)) | (hi_nonzero ? (CF | OF) : 0);
        return true;
    }
    case ZYDIS_MNEMONIC_IMUL: {
        if (in.nops == 1) {
            const s64 a = SignExtend(ReadGpr(RAX, sz), sz), b = SignExtend(Read(o0), sz);
            bool overflow;
            if (sz == 8) {
                const __int128 r = static_cast<__int128>(a) * b;
                s.gpr[RAX] = static_cast<u64>(r);
                s.gpr[RDX] = static_cast<u64>(static_cast<unsigned __int128>(r) >> 64);
                overflow = r != static_cast<s64>(r);
            } else if (sz == 1) {
                const s64 r = a * b;
                WriteGpr(RAX, 2, static_cast<u64>(r));
                overflow = r != static_cast<s8>(r);
            } else {
                const s64 r = a * b;
                WriteGpr(RAX, sz, static_cast<u64>(r));
                WriteGpr(RDX, sz, static_cast<u64>(r) >> (sz * 8));
                overflow = r != SignExtend(static_cast<u64>(r), sz);
            }
            s.rflags = (s.rflags & ~(CF | OF)) | (overflow ? (CF | OF) : 0);
            return true;
        }
        const s64 a = SignExtend(Read(in.nops == 3 ? o1 : o0), sz);
        const s64 b = SignExtend(in.nops == 3 ? static_cast<u64>(in.op[2].value) : Read(o1), sz);
        const __int128 r = static_cast<__int128>(a) * b;
        const u64 lo = static_cast<u64>(r);
        Write(o0, lo);
        const bool overflow = r != static_cast<__int128>(SignExtend(lo, sz));
        SetSzp(lo, sz);
        s.rflags = (s.rflags & ~(CF | OF)) | (overflow ? (CF | OF) : 0);
        return true;
    }
    case ZYDIS_MNEMONIC_DIV: {
        const u64 d = Read(o0);
        if (d == 0) {
            Fault("integer divide by zero (#DE)");
        }
        if (sz == 8) {
            const unsigned __int128 n =
                (static_cast<unsigned __int128>(s.gpr[RDX]) << 64) | s.gpr[RAX];
            const unsigned __int128 q = n / d;
            if (q >> 64) {
                Fault("divide overflow (#DE)");
            }
            s.gpr[RAX] = static_cast<u64>(q);
            s.gpr[RDX] = static_cast<u64>(n % d);
        } else if (sz == 1) {
            const u64 n = ReadGpr(RAX, 2);
            const u64 q = n / d;
            if (q > 0xFF) {
                Fault("divide overflow (#DE)");
            }
            WriteGpr(RAX, 2, (q & 0xFF) | ((n % d) << 8));
        } else {
            const u64 n = (ReadGpr(RDX, sz) << (sz * 8)) | ReadGpr(RAX, sz);
            const u64 q = n / d;
            if (q > Mask(sz)) {
                Fault("divide overflow (#DE)");
            }
            WriteGpr(RAX, sz, q);
            WriteGpr(RDX, sz, n % d);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_IDIV: {
        const s64 d = SignExtend(Read(o0), sz);
        if (d == 0) {
            Fault("integer divide by zero (#DE)");
        }
        if (sz == 8) {
            const __int128 n = static_cast<__int128>(
                (static_cast<unsigned __int128>(s.gpr[RDX]) << 64) | s.gpr[RAX]);
            const __int128 q = n / d;
            if (q != static_cast<s64>(q)) {
                Fault("divide overflow (#DE)");
            }
            s.gpr[RAX] = static_cast<u64>(q);
            s.gpr[RDX] = static_cast<u64>(static_cast<s64>(n % d));
        } else if (sz == 1) {
            const s64 n = static_cast<s16>(ReadGpr(RAX, 2));
            const s64 q = n / d;
            if (q != static_cast<s8>(q)) {
                Fault("divide overflow (#DE)");
            }
            WriteGpr(RAX, 2, (static_cast<u64>(q) & 0xFF) | ((static_cast<u64>(n % d) & 0xFF) << 8));
        } else {
            const s64 n = SignExtend((ReadGpr(RDX, sz) << (sz * 8)) | ReadGpr(RAX, sz), sz * 2);
            const s64 q = n / d;
            if (q != SignExtend(static_cast<u64>(q), sz)) {
                Fault("divide overflow (#DE)");
            }
            WriteGpr(RAX, sz, static_cast<u64>(q));
            WriteGpr(RDX, sz, static_cast<u64>(n % d));
        }
        return true;
    }

    // ---- shifts and rotates ----
    case ZYDIS_MNEMONIC_SHL:
    case ZYDIS_MNEMONIC_SHR:
    case ZYDIS_MNEMONIC_SAR:
    case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR:
    case ZYDIS_MNEMONIC_RCL:
    case ZYDIS_MNEMONIC_RCR: {
        const u8 bits = sz * 8;
        u64 count = (in.nops > 1 ? Read(o1) : 1) & (sz == 8 ? 0x3F : 0x1F);
        const u64 v = Read(o0) & Mask(sz);
        if (count == 0) {
            return true;
        }
        u64 r = v;
        u64 f = s.rflags;
        switch (in.mnemonic) {
        case ZYDIS_MNEMONIC_SHL: {
            r = count >= 64 ? 0 : (v << count) & Mask(sz);
            const bool cf = count <= bits && ((v >> (bits - count)) & 1);
            Write(o0, r);
            SetSzp(r, sz);
            f = (s.rflags & ~(CF | OF | AF)) | (cf ? CF : 0);
            if (((r & SignBit(sz)) != 0) != cf) {
                f |= OF;
            }
            s.rflags = f;
            return true;
        }
        case ZYDIS_MNEMONIC_SHR: {
            r = v >> count;
            const bool cf = (v >> (count - 1)) & 1;
            Write(o0, r);
            SetSzp(r, sz);
            f = (s.rflags & ~(CF | OF | AF)) | (cf ? CF : 0);
            if (v & SignBit(sz)) {
                f |= OF;
            }
            s.rflags = f;
            return true;
        }
        case ZYDIS_MNEMONIC_SAR: {
            const s64 sv = SignExtend(v, sz);
            r = static_cast<u64>(sv >> count) & Mask(sz);
            const bool cf = (sv >> (count - 1)) & 1;
            Write(o0, r);
            SetSzp(r, sz);
            s.rflags = (s.rflags & ~(CF | OF | AF)) | (cf ? CF : 0);
            return true;
        }
        case ZYDIS_MNEMONIC_ROL: {
            const u64 c = count % bits;
            r = c ? ((v << c) | (v >> (bits - c))) & Mask(sz) : v;
            Write(o0, r);
            const bool cf = r & 1;
            f = (s.rflags & ~(CF | OF)) | (cf ? CF : 0);
            if (((r & SignBit(sz)) != 0) != cf) {
                f |= OF;
            }
            s.rflags = f;
            return true;
        }
        case ZYDIS_MNEMONIC_ROR: {
            const u64 c = count % bits;
            r = c ? ((v >> c) | (v << (bits - c))) & Mask(sz) : v;
            Write(o0, r);
            const bool msb = r & SignBit(sz);
            const bool msb2 = r & (SignBit(sz) >> 1);
            f = (s.rflags & ~(CF | OF)) | (msb ? CF : 0) | ((msb != msb2) ? OF : 0);
            s.rflags = f;
            return true;
        }
        default: { // RCL / RCR, one bit at a time through CF
            bool cf = s.rflags & CF;
            const bool left = in.mnemonic == ZYDIS_MNEMONIC_RCL;
            count %= (bits + 1);
            for (u64 i = 0; i < count; i++) {
                if (left) {
                    const bool out = r & SignBit(sz);
                    r = ((r << 1) | (cf ? 1 : 0)) & Mask(sz);
                    cf = out;
                } else {
                    const bool out = r & 1;
                    r = (r >> 1) | (cf ? SignBit(sz) : 0);
                    cf = out;
                }
            }
            Write(o0, r);
            f = (s.rflags & ~(CF | OF)) | (cf ? CF : 0);
            if (left ? (((r & SignBit(sz)) != 0) != cf)
                     : (((r & SignBit(sz)) != 0) != ((r & (SignBit(sz) >> 1)) != 0))) {
                f |= OF;
            }
            s.rflags = f;
            return true;
        }
        }
    }
    case ZYDIS_MNEMONIC_SHLD:
    case ZYDIS_MNEMONIC_SHRD: {
        const u8 bits = sz * 8;
        const u64 count = Read(in.op[2]) & (sz == 8 ? 0x3F : 0x1F);
        if (count == 0) {
            return true;
        }
        const u64 d = Read(o0) & Mask(sz), src = Read(o1) & Mask(sz);
        u64 r;
        bool cf;
        if (in.mnemonic == ZYDIS_MNEMONIC_SHLD) {
            r = ((d << count) | (count >= bits ? 0 : (src >> (bits - count)))) & Mask(sz);
            cf = (d >> (bits - count)) & 1;
        } else {
            r = ((d >> count) | (count >= bits ? 0 : (src << (bits - count)))) & Mask(sz);
            cf = (d >> (count - 1)) & 1;
        }
        Write(o0, r);
        SetSzp(r, sz);
        s.rflags = (s.rflags & ~(CF | OF)) | (cf ? CF : 0) |
                   ((((r ^ d) & SignBit(sz)) != 0) ? OF : 0);
        return true;
    }

    // ---- bit operations ----
    case ZYDIS_MNEMONIC_BT:
    case ZYDIS_MNEMONIC_BTS:
    case ZYDIS_MNEMONIC_BTR:
    case ZYDIS_MNEMONIC_BTC: {
        const u8 bits = sz * 8;
        u64 bit;
        Op target = o0;
        if (o1.kind == OpKind::Imm) {
            bit = static_cast<u64>(o1.value) % bits;
        } else {
            const s64 off = SignExtend(Read(o1), o1.size);
            if (o0.kind == OpKind::Mem) {
                // Register bit offsets index a bit string starting at the memory operand.
                target.value += (off >> (bits == 64 ? 6 : bits == 32 ? 5 : 4)) * sz;
                bit = static_cast<u64>(off) & (bits - 1);
            } else {
                bit = static_cast<u64>(off) % bits;
            }
        }
        const u64 mask = 1ULL << bit;
        const u16 m = in.mnemonic;
        u64 old;
        if (m == ZYDIS_MNEMONIC_BT) {
            old = Read(target);
        } else {
            old = Rmw(*this, in, target, locked, [&](u64 v) {
                return m == ZYDIS_MNEMONIC_BTS ? (v | mask)
                       : m == ZYDIS_MNEMONIC_BTR ? (v & ~mask)
                                                 : (v ^ mask);
            });
        }
        s.rflags = (s.rflags & ~CF) | ((old & mask) ? CF : 0);
        return true;
    }
    case ZYDIS_MNEMONIC_BSF:
    case ZYDIS_MNEMONIC_BSR: {
        const u64 v = Read(o1) & Mask(sz);
        if (v == 0) {
            s.rflags |= ZF;
            return true; // destination left unchanged (AMD documents this)
        }
        s.rflags &= ~ZF;
        Write(o0, in.mnemonic == ZYDIS_MNEMONIC_BSF ? std::countr_zero(v) : 63 - std::countl_zero(v));
        return true;
    }
    case ZYDIS_MNEMONIC_TZCNT:
    case ZYDIS_MNEMONIC_LZCNT:
    case ZYDIS_MNEMONIC_POPCNT: {
        const u64 v = Read(o1) & Mask(sz);
        const u8 bits = sz * 8;
        u64 r;
        if (in.mnemonic == ZYDIS_MNEMONIC_POPCNT) {
            r = std::popcount(v);
            s.rflags &= ~kStatusFlags;
            if (v == 0) {
                s.rflags |= ZF;
            }
        } else {
            r = v == 0 ? bits
                : in.mnemonic == ZYDIS_MNEMONIC_TZCNT ? std::countr_zero(v)
                                                      : std::countl_zero(v) - (64 - bits);
            s.rflags = (s.rflags & ~(CF | ZF)) | (v == 0 ? CF : 0) | (r == 0 ? ZF : 0);
        }
        Write(o0, r);
        return true;
    }
    case ZYDIS_MNEMONIC_ANDN: {
        const u64 r = (~Read(o1) & Read(in.op[2])) & Mask(sz);
        Write(o0, r);
        FlagsLogic(r, sz);
        return true;
    }
    case ZYDIS_MNEMONIC_BEXTR: {
        const u64 src = Read(o1) & Mask(sz), ctl = Read(in.op[2]);
        const u64 start = ctl & 0xFF, len = (ctl >> 8) & 0xFF;
        u64 r = start >= sz * 8 ? 0 : src >> start;
        if (len < 64) {
            r &= (1ULL << len) - 1;
        }
        Write(o0, r);
        FlagsLogic(r, sz);
        return true;
    }
    case ZYDIS_MNEMONIC_BLSI:
    case ZYDIS_MNEMONIC_BLSMSK:
    case ZYDIS_MNEMONIC_BLSR: {
        const u64 v = Read(o1) & Mask(sz);
        const u64 r = (in.mnemonic == ZYDIS_MNEMONIC_BLSI     ? (v & (0 - v))
                       : in.mnemonic == ZYDIS_MNEMONIC_BLSMSK ? (v ^ (v - 1))
                                                              : (v & (v - 1))) &
                      Mask(sz);
        Write(o0, r);
        SetSzp(r, sz);
        s.rflags = (s.rflags & ~(CF | OF)) | (v == 0 ? (in.mnemonic == ZYDIS_MNEMONIC_BLSR ? CF : CF) : 0);
        if (in.mnemonic == ZYDIS_MNEMONIC_BLSI) {
            s.rflags = (s.rflags & ~CF) | (v != 0 ? CF : 0);
        }
        return true;
    }

    // ---- flags ----
    case ZYDIS_MNEMONIC_CLC:
        s.rflags &= ~CF;
        return true;
    case ZYDIS_MNEMONIC_STC:
        s.rflags |= CF;
        return true;
    case ZYDIS_MNEMONIC_CMC:
        s.rflags ^= CF;
        return true;
    case ZYDIS_MNEMONIC_CLD:
        s.rflags &= ~DF;
        return true;
    case ZYDIS_MNEMONIC_STD:
        s.rflags |= DF;
        return true;
    case ZYDIS_MNEMONIC_LAHF:
        WriteGpr(RAX | kHighByte, 1, (s.rflags & (SF | ZF | AF | PF | CF)) | 2);
        return true;
    case ZYDIS_MNEMONIC_SAHF:
        s.rflags = (s.rflags & ~(SF | ZF | AF | PF | CF)) |
                   (((s.gpr[RAX] >> 8) & 0xFF) & (SF | ZF | AF | PF | CF));
        return true;

    // ---- control flow ----
    case ZYDIS_MNEMONIC_JMP:
        s.rip = o0.kind == OpKind::Imm ? next_rip + o0.value : Read(o0);
        return true;
    case ZYDIS_MNEMONIC_CALL: {
        const u64 target = o0.kind == OpKind::Imm ? next_rip + o0.value : Read(o0);
        Push64(next_rip);
        s.rip = target;
        return true;
    }
    case ZYDIS_MNEMONIC_RET: {
        s.rip = Pop64();
        if (in.nops > 0 && o0.kind == OpKind::Imm) {
            s.gpr[RSP] += static_cast<u64>(o0.value);
        }
        return true;
    }
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
    case ZYDIS_MNEMONIC_JNLE: {
        static constexpr u16 order[16] = {
            ZYDIS_MNEMONIC_JO,  ZYDIS_MNEMONIC_JNO, ZYDIS_MNEMONIC_JB,  ZYDIS_MNEMONIC_JNB,
            ZYDIS_MNEMONIC_JZ,  ZYDIS_MNEMONIC_JNZ, ZYDIS_MNEMONIC_JBE, ZYDIS_MNEMONIC_JNBE,
            ZYDIS_MNEMONIC_JS,  ZYDIS_MNEMONIC_JNS, ZYDIS_MNEMONIC_JP,  ZYDIS_MNEMONIC_JNP,
            ZYDIS_MNEMONIC_JL,  ZYDIS_MNEMONIC_JNL, ZYDIS_MNEMONIC_JLE, ZYDIS_MNEMONIC_JNLE};
        int cc = 0;
        while (order[cc] != in.mnemonic) {
            cc++;
        }
        if (Cond(cc)) {
            s.rip = next_rip + o0.value;
        }
        return true;
    }
    case ZYDIS_MNEMONIC_JRCXZ:
    case ZYDIS_MNEMONIC_JECXZ:
        if ((in.mnemonic == ZYDIS_MNEMONIC_JRCXZ ? s.gpr[RCX] : (s.gpr[RCX] & 0xFFFFFFFF)) == 0) {
            s.rip = next_rip + o0.value;
        }
        return true;
    case ZYDIS_MNEMONIC_LOOP:
    case ZYDIS_MNEMONIC_LOOPE:
    case ZYDIS_MNEMONIC_LOOPNE: {
        const u64 c = --s.gpr[RCX];
        bool take = c != 0;
        if (in.mnemonic == ZYDIS_MNEMONIC_LOOPE) {
            take = take && (s.rflags & ZF);
        } else if (in.mnemonic == ZYDIS_MNEMONIC_LOOPNE) {
            take = take && !(s.rflags & ZF);
        }
        if (take) {
            s.rip = next_rip + o0.value;
        }
        return true;
    }
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
    case ZYDIS_MNEMONIC_SETNLE: {
        static constexpr u16 order[16] = {
            ZYDIS_MNEMONIC_SETO, ZYDIS_MNEMONIC_SETNO, ZYDIS_MNEMONIC_SETB,  ZYDIS_MNEMONIC_SETNB,
            ZYDIS_MNEMONIC_SETZ, ZYDIS_MNEMONIC_SETNZ, ZYDIS_MNEMONIC_SETBE, ZYDIS_MNEMONIC_SETNBE,
            ZYDIS_MNEMONIC_SETS, ZYDIS_MNEMONIC_SETNS, ZYDIS_MNEMONIC_SETP,  ZYDIS_MNEMONIC_SETNP,
            ZYDIS_MNEMONIC_SETL, ZYDIS_MNEMONIC_SETNL, ZYDIS_MNEMONIC_SETLE, ZYDIS_MNEMONIC_SETNLE};
        int cc = 0;
        while (order[cc] != in.mnemonic) {
            cc++;
        }
        Write(o0, Cond(cc) ? 1 : 0);
        return true;
    }
    case ZYDIS_MNEMONIC_CMOVO:
    case ZYDIS_MNEMONIC_CMOVNO:
    case ZYDIS_MNEMONIC_CMOVB:
    case ZYDIS_MNEMONIC_CMOVNB:
    case ZYDIS_MNEMONIC_CMOVZ:
    case ZYDIS_MNEMONIC_CMOVNZ:
    case ZYDIS_MNEMONIC_CMOVBE:
    case ZYDIS_MNEMONIC_CMOVNBE:
    case ZYDIS_MNEMONIC_CMOVS:
    case ZYDIS_MNEMONIC_CMOVNS:
    case ZYDIS_MNEMONIC_CMOVP:
    case ZYDIS_MNEMONIC_CMOVNP:
    case ZYDIS_MNEMONIC_CMOVL:
    case ZYDIS_MNEMONIC_CMOVNL:
    case ZYDIS_MNEMONIC_CMOVLE:
    case ZYDIS_MNEMONIC_CMOVNLE: {
        static constexpr u16 order[16] = {
            ZYDIS_MNEMONIC_CMOVO, ZYDIS_MNEMONIC_CMOVNO, ZYDIS_MNEMONIC_CMOVB,
            ZYDIS_MNEMONIC_CMOVNB, ZYDIS_MNEMONIC_CMOVZ, ZYDIS_MNEMONIC_CMOVNZ,
            ZYDIS_MNEMONIC_CMOVBE, ZYDIS_MNEMONIC_CMOVNBE, ZYDIS_MNEMONIC_CMOVS,
            ZYDIS_MNEMONIC_CMOVNS, ZYDIS_MNEMONIC_CMOVP, ZYDIS_MNEMONIC_CMOVNP,
            ZYDIS_MNEMONIC_CMOVL, ZYDIS_MNEMONIC_CMOVNL, ZYDIS_MNEMONIC_CMOVLE,
            ZYDIS_MNEMONIC_CMOVNLE};
        int cc = 0;
        while (order[cc] != in.mnemonic) {
            cc++;
        }
        const u64 v = Read(o1); // the source is read (and can fault) even if not taken
        if (Cond(cc)) {
            Write(o0, v);
        } else if (sz == 4) {
            Write(o0, Read(o0)); // 32-bit CMOV zero-extends its destination regardless
        }
        return true;
    }

    // ---- system ----
    case ZYDIS_MNEMONIC_CPUID:
        Cpuid(s);
        return true;
    case ZYDIS_MNEMONIC_RDTSC:
    case ZYDIS_MNEMONIC_RDTSCP: {
        const u64 t = Common::FencedRDTSC();
        s.gpr[RAX] = t & 0xFFFFFFFF;
        s.gpr[RDX] = t >> 32;
        if (in.mnemonic == ZYDIS_MNEMONIC_RDTSCP) {
            s.gpr[RCX] = 0;
        }
        return true;
    }
    case ZYDIS_MNEMONIC_XGETBV:
        s.gpr[RAX] = 7;
        s.gpr[RDX] = 0;
        return true;
    case ZYDIS_MNEMONIC_RDRAND:
    case ZYDIS_MNEMONIC_RDSEED: {
        static thread_local u64 x = 0x9E3779B97F4A7C15ULL ^ Common::FencedRDTSC();
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        Write(o0, x);
        s.rflags = (s.rflags & ~kStatusFlags) | CF;
        return true;
    }
    case ZYDIS_MNEMONIC_INT3:
        Fault("int3 breakpoint");
    case ZYDIS_MNEMONIC_UD2:
        Fault("ud2 (guest trap)");
    case ZYDIS_MNEMONIC_HLT:
        Fault("hlt");
    case ZYDIS_MNEMONIC_SYSCALL:
        Fault("raw syscall (libkernel is HLE'd; guest code should not syscall directly)");
    default:
        return false;
    }
}

bool Cpu::ExecString(const Insn& in) {
    u8 sz;
    enum Kind { Movs, Stos, Lods, Scas, Cmps } kind;
    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_MOVSB:
        kind = Movs, sz = 1;
        break;
    case ZYDIS_MNEMONIC_MOVSW:
        kind = Movs, sz = 2;
        break;
    case ZYDIS_MNEMONIC_MOVSD:
        kind = Movs, sz = 4;
        break;
    case ZYDIS_MNEMONIC_MOVSQ:
        kind = Movs, sz = 8;
        break;
    case ZYDIS_MNEMONIC_STOSB:
        kind = Stos, sz = 1;
        break;
    case ZYDIS_MNEMONIC_STOSW:
        kind = Stos, sz = 2;
        break;
    case ZYDIS_MNEMONIC_STOSD:
        kind = Stos, sz = 4;
        break;
    case ZYDIS_MNEMONIC_STOSQ:
        kind = Stos, sz = 8;
        break;
    case ZYDIS_MNEMONIC_LODSB:
        kind = Lods, sz = 1;
        break;
    case ZYDIS_MNEMONIC_LODSW:
        kind = Lods, sz = 2;
        break;
    case ZYDIS_MNEMONIC_LODSD:
        kind = Lods, sz = 4;
        break;
    case ZYDIS_MNEMONIC_LODSQ:
        kind = Lods, sz = 8;
        break;
    case ZYDIS_MNEMONIC_SCASB:
        kind = Scas, sz = 1;
        break;
    case ZYDIS_MNEMONIC_SCASW:
        kind = Scas, sz = 2;
        break;
    case ZYDIS_MNEMONIC_SCASD:
        kind = Scas, sz = 4;
        break;
    case ZYDIS_MNEMONIC_SCASQ:
        kind = Scas, sz = 8;
        break;
    case ZYDIS_MNEMONIC_CMPSB:
        kind = Cmps, sz = 1;
        break;
    case ZYDIS_MNEMONIC_CMPSW:
        kind = Cmps, sz = 2;
        break;
    case ZYDIS_MNEMONIC_CMPSD:
        kind = Cmps, sz = 4;
        break;
    case ZYDIS_MNEMONIC_CMPSQ:
        kind = Cmps, sz = 8;
        break;
    default:
        return false;
    }
    const bool rep = in.attrs & (kRep | kRepne);
    const bool back = s.rflags & DF;
    const s64 step = back ? -static_cast<s64>(sz) : sz;
    u64& rsi = s.gpr[RSI];
    u64& rdi = s.gpr[RDI];
    u64& rcx = s.gpr[RCX];

    // Bulk paths for the common forward REP MOVS/STOS (memcpy/memset implementations).
    if (rep && !back && kind == Movs && rcx > 0) {
        const u64 n = rcx * sz;
        u8* d = Ptr(rdi, n);
        const u8* src = Ptr(rsi, n);
        if (d <= src || d >= src + n) {
            std::memmove(d, src, n);
        } else {
            for (u64 i = 0; i < n; i++) { // overlapping forward copy replicates, byte by byte
                d[i] = src[i];
            }
        }
        rsi += n;
        rdi += n;
        rcx = 0;
        return true;
    }
    if (rep && !back && kind == Stos && rcx > 0) {
        const u64 n = rcx * sz;
        u8* d = Ptr(rdi, n);
        if (sz == 1) {
            std::memset(d, static_cast<u8>(s.gpr[RAX]), n);
        } else {
            for (u64 i = 0; i < rcx; i++) {
                std::memcpy(d + i * sz, &s.gpr[RAX], sz);
            }
        }
        rdi += n;
        rcx = 0;
        return true;
    }

    const auto load = [&](u64 a) -> u64 {
        u64 v = 0;
        std::memcpy(&v, Ptr(a, sz), sz);
        return v;
    };
    for (;;) {
        if (rep && rcx == 0) {
            break;
        }
        switch (kind) {
        case Movs:
            std::memcpy(Ptr(rdi, sz), Ptr(rsi, sz), sz);
            rsi += step;
            rdi += step;
            break;
        case Stos:
            std::memcpy(Ptr(rdi, sz), &s.gpr[RAX], sz);
            rdi += step;
            break;
        case Lods:
            WriteGpr(RAX, sz, load(rsi));
            rsi += step;
            break;
        case Scas:
            FlagsSub(ReadGpr(RAX, sz), load(rdi), 0, sz);
            rdi += step;
            break;
        case Cmps:
            FlagsSub(load(rsi), load(rdi), 0, sz);
            rsi += step;
            rdi += step;
            break;
        }
        if (!rep) {
            break;
        }
        rcx--;
        if (kind == Scas || kind == Cmps) {
            const bool zf = s.rflags & ZF;
            if ((in.attrs & kRep) && !zf) {
                break;
            }
            if ((in.attrs & kRepne) && zf) {
                break;
            }
        }
    }
    return true;
}

} // namespace Core::Interpreter
