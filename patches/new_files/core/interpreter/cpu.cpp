// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <bit>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <emscripten/heap.h>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/interpreter/cpu.h"
#include "core/interpreter/hle.h"

namespace Core::Interpreter {

namespace {

thread_local Cpu* tls_cpu = nullptr;

std::array<u16, ZYDIS_MNEMONIC_MAX_VALUE + 1> BuildFoldTable() {
    std::array<u16, ZYDIS_MNEMONIC_MAX_VALUE + 1> table{};
    std::unordered_map<std::string_view, u16> by_name;
    for (u32 m = 0; m <= ZYDIS_MNEMONIC_MAX_VALUE; m++) {
        table[m] = static_cast<u16>(m);
        if (const char* name = ZydisMnemonicGetString(static_cast<ZydisMnemonic>(m))) {
            by_name.emplace(name, static_cast<u16>(m));
        }
    }
    for (u32 m = 0; m <= ZYDIS_MNEMONIC_MAX_VALUE; m++) {
        const char* name = ZydisMnemonicGetString(static_cast<ZydisMnemonic>(m));
        if (name && name[0] == 'v') {
            if (const auto it = by_name.find(name + 1); it != by_name.end()) {
                table[m] = it->second;
            }
        }
    }
    return table;
}

u8 MapReg(ZydisRegister r) {
    if (r == ZYDIS_REGISTER_NONE) {
        return kRegNone;
    }
    if (r >= ZYDIS_REGISTER_AL && r <= ZYDIS_REGISTER_BL) {
        return static_cast<u8>(r - ZYDIS_REGISTER_AL);
    }
    if (r >= ZYDIS_REGISTER_AH && r <= ZYDIS_REGISTER_BH) {
        return static_cast<u8>((r - ZYDIS_REGISTER_AH) | kHighByte);
    }
    if (r >= ZYDIS_REGISTER_SPL && r <= ZYDIS_REGISTER_DIL) {
        return static_cast<u8>(4 + (r - ZYDIS_REGISTER_SPL));
    }
    if (r >= ZYDIS_REGISTER_R8B && r <= ZYDIS_REGISTER_R15B) {
        return static_cast<u8>(8 + (r - ZYDIS_REGISTER_R8B));
    }
    if (r >= ZYDIS_REGISTER_AX && r <= ZYDIS_REGISTER_R15W) {
        return static_cast<u8>(r - ZYDIS_REGISTER_AX);
    }
    if (r >= ZYDIS_REGISTER_EAX && r <= ZYDIS_REGISTER_R15D) {
        return static_cast<u8>(r - ZYDIS_REGISTER_EAX);
    }
    if (r >= ZYDIS_REGISTER_RAX && r <= ZYDIS_REGISTER_R15) {
        return static_cast<u8>(r - ZYDIS_REGISTER_RAX);
    }
    if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7) {
        return static_cast<u8>(kRegSt + (r - ZYDIS_REGISTER_ST0));
    }
    if (r >= ZYDIS_REGISTER_MM0 && r <= ZYDIS_REGISTER_MM7) {
        return static_cast<u8>(kRegMmx + (r - ZYDIS_REGISTER_MM0));
    }
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15) {
        return static_cast<u8>(kRegVec + (r - ZYDIS_REGISTER_XMM0));
    }
    if (r >= ZYDIS_REGISTER_YMM0 && r <= ZYDIS_REGISTER_YMM15) {
        return static_cast<u8>(kRegVec + (r - ZYDIS_REGISTER_YMM0));
    }
    if (r >= ZYDIS_REGISTER_ES && r <= ZYDIS_REGISTER_GS) {
        return static_cast<u8>(kRegSeg + (r - ZYDIS_REGISTER_ES));
    }
    if (r == ZYDIS_REGISTER_RIP || r == ZYDIS_REGISTER_EIP) {
        return kRegRip;
    }
    return 0xFD;
}

Op ConvertOperand(const ZydisDecodedOperand& z) {
    Op o;
    o.size = static_cast<u8>(z.size / 8);
    switch (z.type) {
    case ZYDIS_OPERAND_TYPE_REGISTER:
        o.kind = OpKind::Reg;
        o.reg = MapReg(z.reg.value);
        break;
    case ZYDIS_OPERAND_TYPE_MEMORY:
        o.kind = OpKind::Mem;
        o.base = MapReg(z.mem.base);
        o.index = MapReg(z.mem.index);
        o.scale = z.mem.scale;
        o.value = z.mem.disp.value;
        o.seg = z.mem.segment == ZYDIS_REGISTER_FS ? 1 : z.mem.segment == ZYDIS_REGISTER_GS ? 2 : 0;
        break;
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        o.kind = OpKind::Imm;
        o.imm_signed = z.imm.is_signed;
        o.value = z.imm.is_signed ? z.imm.value.s : static_cast<s64>(z.imm.value.u);
        break;
    default:
        break;
    }
    return o;
}

const std::array<u16, ZYDIS_MNEMONIC_MAX_VALUE + 1>& FoldTable() {
    static const auto table = BuildFoldTable();
    return table;
}

} // namespace

u16 FoldMnemonic(ZydisMnemonic m) {
    return FoldTable()[m];
}

Cpu::Cpu() : cache{std::make_unique<CacheEntry[]>(1u << kCacheBits)} {
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    mem_limit = emscripten_get_heap_size();
    FoldTable();
}

Cpu::~Cpu() {
    if (tls_cpu == this) {
        tls_cpu = nullptr;
    }
}

Cpu& Cpu::Current() {
    if (!tls_cpu) [[unlikely]] {
        // Deliberately never freed: guest threads exit through paths (posix_pthread_exit) that
        // don't unwind back to whoever would own it, and a thread's Cpu is small.
        tls_cpu = new Cpu();
    }
    return *tls_cpu;
}

Cpu* Cpu::CurrentIfAny() {
    return tls_cpu;
}

u8* Cpu::SlowPtr(u64 addr, u64 size) {
    // Linear memory may have grown since this thread last looked.
    mem_limit = emscripten_get_heap_size();
    if (addr >= kMinAddress && addr + size <= mem_limit && addr + size >= addr) {
        return reinterpret_cast<u8*>(addr);
    }
    Fault("guest memory access out of range", addr);
}

void Cpu::Fault(const char* what, u64 addr) const {
    throw GuestFault(fmt::format("{} (address {:#x}) at rip {:#x}: {}", what, addr, s.rip,
                                 Disassemble(s.rip)));
}

void Cpu::Unimplemented(const Insn& in) const {
    throw GuestFault(fmt::format("unimplemented instruction at {:#x}: {}", s.rip,
                                 Disassemble(s.rip)));
}

std::string Disassemble(u64 addr) {
    if (addr < 0x10000 || addr >= kThunkBase) {
        return "<no code>";
    }
    ZydisDisassembledInstruction d;
    if (ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, addr,
                                           reinterpret_cast<const void*>(addr), 15, &d))) {
        std::string bytes;
        for (u8 i = 0; i < d.info.length; i++) {
            bytes += fmt::format("{:02x}", reinterpret_cast<const u8*>(addr)[i]);
        }
        return fmt::format("{} [{}]", d.text, bytes);
    }
    return "<undecodable>";
}

const Insn* Cpu::Decode(u64 rip) {
    CacheEntry& e = cache[(rip ^ (rip >> kCacheBits)) & ((1u << kCacheBits) - 1)];
    const u8* code = Ptr(rip, 16);
    u64 cur[2];
    std::memcpy(cur, code, 16);
    if (e.rip == rip) {
        // Self-validating: the cached decode is only reused if the bytes it came from are
        // unchanged, so code written at runtime (JITs, patched modules) is never stale.
        const u8 len = e.insn.length;
        const u64 m0 = len >= 8 ? ~0ULL : ((1ULL << (len * 8)) - 1);
        const u64 m1 = len >= 16 ? ~0ULL : len <= 8 ? 0 : ((1ULL << ((len - 8) * 8)) - 1);
        if (((cur[0] ^ e.bytes[0]) & m0) == 0 && ((cur[1] ^ e.bytes[1]) & m1) == 0) {
            return &e.insn;
        }
    }

    ZydisDecodedInstruction zi;
    ZydisDecodedOperand zo[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, 15, &zi, zo))) {
        s.rip = rip;
        Fault("undecodable instruction", rip);
    }
    Insn& in = e.insn;
    in = {};
    in.raw_mnemonic = static_cast<u16>(zi.mnemonic);
    in.mnemonic = FoldMnemonic(zi.mnemonic);
    in.length = zi.length;
    in.opsize = static_cast<u8>(zi.operand_width / 8);
    in.nops = std::min<u8>(zi.operand_count_visible, 4);
    if (zi.attributes & ZYDIS_ATTRIB_HAS_LOCK) {
        in.attrs |= kLock;
    }
    if (zi.attributes & (ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE)) {
        in.attrs |= kRep;
    }
    if (zi.attributes & ZYDIS_ATTRIB_HAS_REPNE) {
        in.attrs |= kRepne;
    }
    if (zi.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX) {
        in.attrs |= kVex;
        if (zi.avx.vector_length == 256) {
            in.attrs |= kVex256;
        }
    }
    if (zi.address_width == 32) {
        in.attrs |= kAddr32;
    }
    for (u8 i = 0; i < in.nops; i++) {
        in.op[i] = ConvertOperand(zo[i]);
    }
    e.rip = rip;
    e.bytes[0] = cur[0];
    e.bytes[1] = cur[1];
    return &in;
}

void Cpu::InvalidateCode(u64 addr, u64 size) {
    for (size_t i = 0; i < (1u << kCacheBits); i++) {
        if (cache[i].rip >= addr && cache[i].rip < addr + size) {
            cache[i].rip = ~0ULL;
        }
    }
}

void Cpu::RunUntil(u64 return_address) {
    for (;;) {
        const u64 rip = s.rip;
        if (rip >= kThunkBase) [[unlikely]] {
            if (rip == return_address) {
                return;
            }
            InvokeThunk(*this, rip);
            continue;
        }
        const Insn* in = Decode(rip);
        next_rip = rip + in->length;
        Step(*in);
        icount++;
    }
}

void Cpu::Step(const Insn& in) {
    s.rip = next_rip;
    if (ExecInteger(in) || ExecSimd(in) || ExecX87(in) || ExecString(in)) {
        return;
    }
    s.rip = next_rip - in.length;
    Unimplemented(in);
}

// ---- operands -------------------------------------------------------------------------------

u64 Cpu::EffectiveAddress(const Op& o) const {
    u64 a = static_cast<u64>(o.value);
    if (o.base == kRegRip) {
        a += next_rip;
    } else if (o.base != kRegNone) {
        a += s.gpr[o.base & 15];
    }
    if (o.index != kRegNone) {
        a += s.gpr[o.index & 15] * o.scale;
    }
    if (o.seg == 1) {
        a += s.fs_base;
    } else if (o.seg == 2) {
        a += s.gs_base;
    }
    return a;
}

u64 Cpu::ReadGpr(u8 reg, u8 size) const {
    if (reg & kHighByte) {
        return (s.gpr[reg & 3] >> 8) & 0xFF;
    }
    const u64 v = s.gpr[reg & 15];
    switch (size) {
    case 1:
        return v & 0xFF;
    case 2:
        return v & 0xFFFF;
    case 4:
        return v & 0xFFFFFFFF;
    default:
        return v;
    }
}

void Cpu::WriteGpr(u8 reg, u8 size, u64 v) {
    if (reg & kHighByte) {
        u64& r = s.gpr[reg & 3];
        r = (r & ~0xFF00ULL) | ((v & 0xFF) << 8);
        return;
    }
    u64& r = s.gpr[reg & 15];
    switch (size) {
    case 1:
        r = (r & ~0xFFULL) | (v & 0xFF);
        break;
    case 2:
        r = (r & ~0xFFFFULL) | (v & 0xFFFF);
        break;
    case 4:
        r = v & 0xFFFFFFFF; // 32-bit writes zero the upper half
        break;
    default:
        r = v;
        break;
    }
}

u64 Cpu::Read(const Op& o) {
    switch (o.kind) {
    case OpKind::Reg:
        if (o.reg < 32) {
            return ReadGpr(o.reg, o.size);
        }
        if (o.reg >= kRegVec && o.reg < kRegVec + 16) {
            return s.ymm[o.reg - kRegVec].q[0];
        }
        if (o.reg >= kRegSeg && o.reg < kRegSeg + 6) {
            return 0;
        }
        Fault("unsupported register operand");
    case OpKind::Mem: {
        const u64 a = EffectiveAddress(o);
        switch (o.size) {
        case 1:
            return Load<u8>(a);
        case 2:
            return Load<u16>(a);
        case 4:
            return Load<u32>(a);
        default:
            return Load<u64>(a);
        }
    }
    case OpKind::Imm:
        return static_cast<u64>(o.value);
    default:
        Fault("missing operand");
    }
}

void Cpu::Write(const Op& o, u64 v) {
    if (o.kind == OpKind::Reg) {
        if (o.reg < 32) {
            WriteGpr(o.reg, o.size, v);
            return;
        }
        if (o.reg >= kRegSeg && o.reg < kRegSeg + 6) {
            return; // segment register loads are meaningless in a flat 64-bit address space
        }
        Fault("unsupported register destination");
    }
    if (o.kind != OpKind::Mem) {
        Fault("write to non-memory, non-register operand");
    }
    const u64 a = EffectiveAddress(o);
    switch (o.size) {
    case 1:
        Store<u8>(a, static_cast<u8>(v));
        break;
    case 2:
        Store<u16>(a, static_cast<u16>(v));
        break;
    case 4:
        Store<u32>(a, static_cast<u32>(v));
        break;
    default:
        Store<u64>(a, v);
        break;
    }
}

Vec Cpu::ReadVec(const Op& o) {
    Vec v{};
    switch (o.kind) {
    case OpKind::Reg:
        if (o.reg >= kRegVec && o.reg < kRegVec + 16) {
            v = s.ymm[o.reg - kRegVec];
            if (o.size <= 16) {
                v.q[2] = v.q[3] = 0;
            }
        } else if (o.reg < 32) {
            v.q[0] = ReadGpr(o.reg, o.size);
        } else {
            Fault("unsupported vector operand register");
        }
        return v;
    case OpKind::Mem:
        std::memcpy(&v, Ptr(EffectiveAddress(o), o.size), o.size);
        return v;
    case OpKind::Imm:
        v.q[0] = static_cast<u64>(o.value);
        return v;
    default:
        Fault("missing vector operand");
    }
}

// Writes `size` bytes of `v` to a vector register or memory. For a register destination, legacy
// SSE encodings leave bits above the written width alone; VEX encodings zero everything above.
void Cpu::WriteVec(const Insn& in, const Op& o, const Vec& v, u8 size) {
    if (o.kind == OpKind::Mem) {
        std::memcpy(Ptr(EffectiveAddress(o), size), &v, size);
        return;
    }
    if (o.kind == OpKind::Reg && o.reg >= kRegVec && o.reg < kRegVec + 16) {
        Vec& d = s.ymm[o.reg - kRegVec];
        if (in.attrs & kVex) {
            Vec out{};
            std::memcpy(&out, &v, size);
            d = out;
        } else {
            std::memcpy(&d, &v, size);
        }
        return;
    }
    if (o.kind == OpKind::Reg && o.reg < 32) {
        WriteGpr(o.reg, o.size, v.q[0]);
        return;
    }
    Fault("unsupported vector destination");
}

// ---- flags ----------------------------------------------------------------------------------

namespace {
constexpr u64 Mask(u8 size) {
    return size >= 8 ? ~0ULL : (1ULL << (size * 8)) - 1;
}
constexpr u64 SignBit(u8 size) {
    return 1ULL << (size * 8 - 1);
}
} // namespace

void Cpu::SetSzp(u64 r, u8 size) {
    u64 f = s.rflags & ~(ZF | SF | PF);
    r &= Mask(size);
    if (r == 0) {
        f |= ZF;
    }
    if (r & SignBit(size)) {
        f |= SF;
    }
    if ((std::popcount(static_cast<u8>(r)) & 1) == 0) {
        f |= PF;
    }
    s.rflags = f;
}

u64 Cpu::FlagsAdd(u64 a, u64 b, u64 carry, u8 size) {
    const u64 m = Mask(size);
    a &= m;
    b &= m;
    const u64 r = (a + b + carry) & m;
    SetSzp(r, size);
    u64 f = s.rflags & ~(CF | OF | AF);
    if (size == 8 ? (r < a || (carry && r == a)) : ((a + b + carry) > m)) {
        f |= CF;
    }
    if (((a ^ r) & (b ^ r)) & SignBit(size)) {
        f |= OF;
    }
    if ((a ^ b ^ r) & 0x10) {
        f |= AF;
    }
    s.rflags = f;
    return r;
}

u64 Cpu::FlagsSub(u64 a, u64 b, u64 borrow, u8 size) {
    const u64 m = Mask(size);
    a &= m;
    b &= m;
    const u64 r = (a - b - borrow) & m;
    SetSzp(r, size);
    u64 f = s.rflags & ~(CF | OF | AF);
    if (a < b || (borrow && a == b)) {
        f |= CF;
    }
    if (((a ^ b) & (a ^ r)) & SignBit(size)) {
        f |= OF;
    }
    if ((a ^ b ^ r) & 0x10) {
        f |= AF;
    }
    s.rflags = f;
    return r;
}

u64 Cpu::FlagsLogic(u64 r, u8 size) {
    r &= Mask(size);
    SetSzp(r, size);
    s.rflags &= ~(CF | OF | AF);
    return r;
}

bool Cpu::Cond(int cc) const {
    const u64 f = s.rflags;
    const bool cf = f & CF, zf = f & ZF, sf = f & SF, of = f & OF, pf = f & PF;
    switch (cc) {
    case 0x0:
        return of;
    case 0x1:
        return !of;
    case 0x2:
        return cf;
    case 0x3:
        return !cf;
    case 0x4:
        return zf;
    case 0x5:
        return !zf;
    case 0x6:
        return cf || zf;
    case 0x7:
        return !cf && !zf;
    case 0x8:
        return sf;
    case 0x9:
        return !sf;
    case 0xA:
        return pf;
    case 0xB:
        return !pf;
    case 0xC:
        return sf != of;
    case 0xD:
        return sf == of;
    case 0xE:
        return zf || (sf != of);
    default:
        return !zf && (sf == of);
    }
}

} // namespace Core::Interpreter
