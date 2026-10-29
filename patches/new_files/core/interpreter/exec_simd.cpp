// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SSE, SSE2, SSE3, SSSE3, SSE4.1, SSE4.2, AVX (Jaguar has no AVX2), F16C, AES-NI and PCLMULQDQ.
//
// Legacy and VEX encodings share handlers (decode.cpp folds VADDPS onto ADDPS). Operands are
// normalized per instruction:
//   dst  op[0]
//   B    the last non-immediate operand (the r/m source)
//   A    op[1] if there are three or more non-immediate operands (VEX's extra source), else op[0]
// which covers both "addps x1, x2" (A = x1) and "vaddps x1, x2, x3" (A = x2). Register results
// are written through WriteVec, which preserves bits above 128 for legacy encodings and zeroes them
// for VEX ones.

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include "core/interpreter/cpu.h"

namespace Core::Interpreter {

namespace {

template <typename T>
T SatS(s64 v) {
    return static_cast<T>(std::clamp<s64>(v, std::numeric_limits<T>::min(), std::numeric_limits<T>::max()));
}
template <typename T>
T SatU(s64 v) {
    return static_cast<T>(std::clamp<s64>(v, 0, std::numeric_limits<T>::max()));
}

bool CmpPred(double a, double b, int p) {
    const bool un = std::isnan(a) || std::isnan(b);
    switch (p & 0xF) {
    case 0:
        return !un && a == b;
    case 1:
        return !un && a < b;
    case 2:
        return !un && a <= b;
    case 3:
        return un;
    case 4:
        return un || a != b;
    case 5:
        return un || !(a < b);
    case 6:
        return un || !(a <= b);
    case 7:
        return !un;
    case 8:
        return un || a == b;
    case 9:
        return un || a < b;
    case 10:
        return un || a <= b;
    case 11:
        return false;
    case 12:
        return !un && a != b;
    case 13:
        return !un && a >= b;
    case 14:
        return !un && a > b;
    default:
        return true;
    }
}

template <typename F>
F RoundMode(F v, int imm, u32 mxcsr) {
    const int mode = (imm & 4) ? ((mxcsr >> 13) & 3) : (imm & 3);
    switch (mode) {
    case 0:
        return std::nearbyint(v);
    case 1:
        return std::floor(v);
    case 2:
        return std::ceil(v);
    default:
        return std::trunc(v);
    }
}

// Float -> integer with x86's "integer indefinite" result for NaN/out of range.
template <typename I, typename F>
I CvtInt(F v, bool truncate, u32 mxcsr) {
    if (!truncate) {
        v = RoundMode(v, 4, mxcsr);
    } else {
        v = std::trunc(v);
    }
    constexpr F lo = static_cast<F>(std::numeric_limits<I>::min());
    constexpr F hi = -static_cast<F>(std::numeric_limits<I>::min()); // 2^(bits-1), exact
    if (std::isnan(v) || v < lo || v >= hi) {
        return std::numeric_limits<I>::min();
    }
    return static_cast<I>(v);
}

float HalfToFloat(u16 h) {
    const u32 sign = (h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3FF;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    return std::bit_cast<float>(bits);
}

u16 FloatToHalf(float f, int imm, u32 mxcsr) {
    const u32 x = std::bit_cast<u32>(f);
    const u16 sign = static_cast<u16>((x >> 16) & 0x8000);
    const int exp = static_cast<int>((x >> 23) & 0xFF);
    u32 mant = x & 0x7FFFFF;
    if (exp == 0xFF) {
        return sign | 0x7C00 | (mant ? (0x200 | (mant >> 13)) : 0);
    }
    // Round the magnitude with the requested mode by scaling into half's integer domain.
    const double a = std::fabs(static_cast<double>(f));
    if (a == 0) {
        return sign;
    }
    int e;
    std::frexp(a, &e); // a = m * 2^e, m in [0.5,1)
    int he = e - 1 + 15;
    double scaled;
    if (he <= 0) {
        scaled = std::ldexp(a, 24); // subnormal half: units of 2^-24
        he = 0;
    } else {
        scaled = std::ldexp(a, 10 - (e - 1)) ; // 1.xxx * 2^10
    }
    const int mode = (imm & 4) ? ((mxcsr >> 13) & 3) : (imm & 3);
    double r;
    switch (mode) {
    case 0:
        r = std::nearbyint(scaled);
        break;
    case 1:
        r = sign ? std::ceil(scaled) : std::floor(scaled);
        break;
    case 2:
        r = sign ? std::floor(scaled) : std::ceil(scaled);
        break;
    default:
        r = std::trunc(scaled);
        break;
    }
    u32 m = static_cast<u32>(r);
    if (he == 0) {
        if (m >= 0x400) {
            return sign | 0x400 | (m & 0x3FF);
        }
        return sign | static_cast<u16>(m);
    }
    if (m >= 0x800) { // rounding carried into the next binade
        m >>= 1;
        he++;
    }
    if (he >= 31) {
        return sign | 0x7C00;
    }
    return sign | static_cast<u16>(he << 10) | static_cast<u16>(m & 0x3FF);
}

// ---- AES ----
constexpr u8 kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

u8 InvSbox(u8 v) {
    static const auto table = [] {
        std::array<u8, 256> t{};
        for (int i = 0; i < 256; i++) {
            t[kSbox[i]] = static_cast<u8>(i);
        }
        return t;
    }();
    return table[v];
}

u8 Xtime(u8 x) {
    return static_cast<u8>((x << 1) ^ ((x & 0x80) ? 0x1B : 0));
}
u8 GMul(u8 a, u8 b) {
    u8 r = 0;
    while (b) {
        if (b & 1) {
            r ^= a;
        }
        a = Xtime(a);
        b >>= 1;
    }
    return r;
}

void ShiftRows(u8* st, bool inverse) {
    u8 t[16];
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            const int src = inverse ? ((c - r + 4) % 4) : ((c + r) % 4);
            t[c * 4 + r] = st[src * 4 + r];
        }
    }
    std::memcpy(st, t, 16);
}

void MixColumns(u8* st, bool inverse) {
    for (int c = 0; c < 4; c++) {
        u8* col = st + c * 4;
        const u8 a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        if (!inverse) {
            col[0] = GMul(a0, 2) ^ GMul(a1, 3) ^ a2 ^ a3;
            col[1] = a0 ^ GMul(a1, 2) ^ GMul(a2, 3) ^ a3;
            col[2] = a0 ^ a1 ^ GMul(a2, 2) ^ GMul(a3, 3);
            col[3] = GMul(a0, 3) ^ a1 ^ a2 ^ GMul(a3, 2);
        } else {
            col[0] = GMul(a0, 14) ^ GMul(a1, 11) ^ GMul(a2, 13) ^ GMul(a3, 9);
            col[1] = GMul(a0, 9) ^ GMul(a1, 14) ^ GMul(a2, 11) ^ GMul(a3, 13);
            col[2] = GMul(a0, 13) ^ GMul(a1, 9) ^ GMul(a2, 14) ^ GMul(a3, 11);
            col[3] = GMul(a0, 11) ^ GMul(a1, 13) ^ GMul(a2, 9) ^ GMul(a3, 14);
        }
    }
}

u32 Crc32cByte(u32 crc, u8 b) {
    crc ^= b;
    for (int k = 0; k < 8; k++) {
        crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1)));
    }
    return crc;
}

} // namespace

bool Cpu::ExecSimd(const Insn& in) {
    const u16 m = in.mnemonic;
    const bool vex = in.attrs & kVex;
    const u8 vl = (in.attrs & kVex256) ? 32 : 16;

    // Operand normalization (see the file comment).
    int nr = 0;
    const Op* imm_op = nullptr;
    for (u8 i = 0; i < in.nops; i++) {
        if (in.op[i].kind == OpKind::Imm) {
            imm_op = &in.op[i];
        } else {
            nr++;
        }
    }
    const Op& dst = in.op[0];
    const Op& A = nr >= 3 ? in.op[1] : in.op[0];
    const Op& B = in.op[nr > 0 ? nr - 1 : 0];
    const int imm = imm_op ? static_cast<int>(imm_op->value & 0xFF) : 0;
    const auto is_vec = [](const Op& o) {
        return o.kind == OpKind::Reg && o.reg >= kRegVec && o.reg < kRegVec + 16;
    };
    bool any_vec = false;
    for (u8 i = 0; i < in.nops; i++) {
        any_vec |= is_vec(in.op[i]);
    }

    const auto out = [&](const Vec& r, u8 size) { WriteVec(in, dst, r, size); };

    // Lane-wise helpers. T is the lane type; `n` lanes = vl / sizeof(T).
#define PACKED(T, field, expr)                                                                     \
    do {                                                                                           \
        const Vec a = ReadVec(A), b = ReadVec(B);                                                  \
        Vec r{};                                                                                   \
        for (int i = 0; i < vl / static_cast<int>(sizeof(T)); i++) {                              \
            const T x = a.field[i], y = b.field[i];                                                \
            (void)x;                                                                               \
            (void)y;                                                                               \
            r.field[i] = static_cast<T>(expr);                                                     \
        }                                                                                          \
        out(r, vl);                                                                                \
        return true;                                                                               \
    } while (0)
#define SCALAR(T, field, expr)                                                                     \
    do {                                                                                           \
        Vec r = ReadVec(A);                                                                        \
        const Vec b = ReadVec(B);                                                                  \
        const T x = r.field[0], y = b.field[0];                                                    \
        (void)x;                                                                                   \
        (void)y;                                                                                   \
        r.field[0] = static_cast<T>(expr);                                                         \
        out(r, 16);                                                                                \
        return true;                                                                               \
    } while (0)
#define UNARY_PACKED(T, field, expr)                                                               \
    do {                                                                                           \
        const Vec b = ReadVec(B);                                                                  \
        Vec r{};                                                                                   \
        for (int i = 0; i < vl / static_cast<int>(sizeof(T)); i++) {                              \
            const T x = b.field[i];                                                                \
            r.field[i] = static_cast<T>(expr);                                                     \
        }                                                                                          \
        out(r, vl);                                                                                \
        return true;                                                                               \
    } while (0)

    switch (m) {
    // ---- moves ----
    case ZYDIS_MNEMONIC_MOVAPS:
    case ZYDIS_MNEMONIC_MOVUPS:
    case ZYDIS_MNEMONIC_MOVAPD:
    case ZYDIS_MNEMONIC_MOVUPD:
    case ZYDIS_MNEMONIC_MOVDQA:
    case ZYDIS_MNEMONIC_MOVDQU:
    case ZYDIS_MNEMONIC_LDDQU:
    case ZYDIS_MNEMONIC_MOVNTPS:
    case ZYDIS_MNEMONIC_MOVNTPD:
    case ZYDIS_MNEMONIC_MOVNTDQ:
    case ZYDIS_MNEMONIC_MOVNTDQA:
        out(ReadVec(B), vl);
        return true;
    case ZYDIS_MNEMONIC_MOVSS:
    case ZYDIS_MNEMONIC_MOVSD: {
        if (!any_vec) {
            return false; // MOVSD the string instruction
        }
        const u8 w = m == ZYDIS_MNEMONIC_MOVSS ? 4 : 8;
        if (dst.kind == OpKind::Mem) {
            WriteVec(in, dst, ReadVec(B), w);
        } else if (B.kind == OpKind::Mem) {
            Vec r{};
            std::memcpy(&r, Ptr(EffectiveAddress(B), w), w);
            // Loads zero the rest of the register (bits 127:w*8 for legacy SSE too).
            if (vex) {
                out(r, 16);
            } else {
                std::memcpy(&s.ymm[dst.reg - kRegVec], &r, 16);
            }
        } else {
            Vec r = ReadVec(A);
            const Vec b = ReadVec(B);
            std::memcpy(&r, &b, w);
            out(r, 16);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_MOVD:
    case ZYDIS_MNEMONIC_MOVQ: {
        const u8 w = m == ZYDIS_MNEMONIC_MOVD ? 4 : 8;
        if (is_vec(dst)) {
            Vec r{};
            if (is_vec(B)) {
                r.q[0] = s.ymm[B.reg - kRegVec].q[0];
            } else if (B.kind == OpKind::Mem) {
                std::memcpy(&r, Ptr(EffectiveAddress(B), w), w);
            } else {
                r.q[0] = ReadGpr(B.reg, w);
            }
            // MOVD/MOVQ into an XMM register zero bits 127:w*8 even when legacy-encoded.
            if (vex) {
                out(r, 16);
            } else {
                std::memcpy(&s.ymm[dst.reg - kRegVec], &r, 16);
            }
        } else {
            const u64 v = s.ymm[B.reg - kRegVec].q[0];
            if (dst.kind == OpKind::Mem) {
                std::memcpy(Ptr(EffectiveAddress(dst), w), &v, w);
            } else {
                WriteGpr(dst.reg, dst.size, w == 4 ? (v & 0xFFFFFFFF) : v);
            }
        }
        return true;
    }
    case ZYDIS_MNEMONIC_MOVLPS:
    case ZYDIS_MNEMONIC_MOVLPD:
    case ZYDIS_MNEMONIC_MOVHPS:
    case ZYDIS_MNEMONIC_MOVHPD: {
        const bool high = m == ZYDIS_MNEMONIC_MOVHPS || m == ZYDIS_MNEMONIC_MOVHPD;
        if (dst.kind == OpKind::Mem) {
            const Vec v = ReadVec(B);
            Store<u64>(EffectiveAddress(dst), v.q[high ? 1 : 0]);
        } else {
            Vec r = ReadVec(A);
            r.q[high ? 1 : 0] = Load<u64>(EffectiveAddress(B));
            out(r, 16);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_MOVHLPS: {
        Vec r = ReadVec(A);
        r.q[0] = ReadVec(B).q[1];
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_MOVLHPS: {
        Vec r = ReadVec(A);
        r.q[1] = ReadVec(B).q[0];
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_MOVDDUP: {
        const Vec b = ReadVec(B);
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            r.q[l * 2] = r.q[l * 2 + 1] = b.q[l * 2];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_MOVSLDUP:
    case ZYDIS_MNEMONIC_MOVSHDUP: {
        const Vec b = ReadVec(B);
        const int o = m == ZYDIS_MNEMONIC_MOVSHDUP ? 1 : 0;
        Vec r{};
        for (int i = 0; i < vl / 4; i += 2) {
            r.d[i] = r.d[i + 1] = b.d[i + o];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_MOVMSKPS:
    case ZYDIS_MNEMONIC_MOVMSKPD:
    case ZYDIS_MNEMONIC_PMOVMSKB: {
        const Vec b = ReadVec(B);
        const u8 w = m == ZYDIS_MNEMONIC_MOVMSKPS ? 4 : m == ZYDIS_MNEMONIC_MOVMSKPD ? 8 : 1;
        u64 r = 0;
        for (int i = 0; i < vl / w; i++) {
            r |= static_cast<u64>((b.b[i * w + w - 1] >> 7) & 1) << i;
        }
        WriteGpr(dst.reg, dst.size, r);
        return true;
    }

    // ---- float arithmetic ----
    case ZYDIS_MNEMONIC_ADDPS:
        PACKED(float, f, x + y);
    case ZYDIS_MNEMONIC_ADDPD:
        PACKED(double, fd, x + y);
    case ZYDIS_MNEMONIC_ADDSS:
        SCALAR(float, f, x + y);
    case ZYDIS_MNEMONIC_ADDSD:
        SCALAR(double, fd, x + y);
    case ZYDIS_MNEMONIC_SUBPS:
        PACKED(float, f, x - y);
    case ZYDIS_MNEMONIC_SUBPD:
        PACKED(double, fd, x - y);
    case ZYDIS_MNEMONIC_SUBSS:
        SCALAR(float, f, x - y);
    case ZYDIS_MNEMONIC_SUBSD:
        SCALAR(double, fd, x - y);
    case ZYDIS_MNEMONIC_MULPS:
        PACKED(float, f, x * y);
    case ZYDIS_MNEMONIC_MULPD:
        PACKED(double, fd, x * y);
    case ZYDIS_MNEMONIC_MULSS:
        SCALAR(float, f, x * y);
    case ZYDIS_MNEMONIC_MULSD:
        SCALAR(double, fd, x * y);
    case ZYDIS_MNEMONIC_DIVPS:
        PACKED(float, f, x / y);
    case ZYDIS_MNEMONIC_DIVPD:
        PACKED(double, fd, x / y);
    case ZYDIS_MNEMONIC_DIVSS:
        SCALAR(float, f, x / y);
    case ZYDIS_MNEMONIC_DIVSD:
        SCALAR(double, fd, x / y);
    // MIN/MAX return the second operand when either is NaN or both are zero.
    case ZYDIS_MNEMONIC_MINPS:
        PACKED(float, f, x < y ? x : y);
    case ZYDIS_MNEMONIC_MINPD:
        PACKED(double, fd, x < y ? x : y);
    case ZYDIS_MNEMONIC_MINSS:
        SCALAR(float, f, x < y ? x : y);
    case ZYDIS_MNEMONIC_MINSD:
        SCALAR(double, fd, x < y ? x : y);
    case ZYDIS_MNEMONIC_MAXPS:
        PACKED(float, f, x > y ? x : y);
    case ZYDIS_MNEMONIC_MAXPD:
        PACKED(double, fd, x > y ? x : y);
    case ZYDIS_MNEMONIC_MAXSS:
        SCALAR(float, f, x > y ? x : y);
    case ZYDIS_MNEMONIC_MAXSD:
        SCALAR(double, fd, x > y ? x : y);
    case ZYDIS_MNEMONIC_SQRTPS:
        UNARY_PACKED(float, f, std::sqrt(x));
    case ZYDIS_MNEMONIC_SQRTPD:
        UNARY_PACKED(double, fd, std::sqrt(x));
    case ZYDIS_MNEMONIC_SQRTSS:
        SCALAR(float, f, std::sqrt(y));
    case ZYDIS_MNEMONIC_SQRTSD:
        SCALAR(double, fd, std::sqrt(y));
    case ZYDIS_MNEMONIC_RCPPS:
        UNARY_PACKED(float, f, 1.0f / x);
    case ZYDIS_MNEMONIC_RCPSS:
        SCALAR(float, f, 1.0f / y);
    case ZYDIS_MNEMONIC_RSQRTPS:
        UNARY_PACKED(float, f, 1.0f / std::sqrt(x));
    case ZYDIS_MNEMONIC_RSQRTSS:
        SCALAR(float, f, 1.0f / std::sqrt(y));
    case ZYDIS_MNEMONIC_ADDSUBPS: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 4; i++) {
            r.f[i] = (i & 1) ? a.f[i] + b.f[i] : a.f[i] - b.f[i];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_ADDSUBPD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 8; i++) {
            r.fd[i] = (i & 1) ? a.fd[i] + b.fd[i] : a.fd[i] - b.fd[i];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_HADDPS:
    case ZYDIS_MNEMONIC_HSUBPS: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const bool sub = m == ZYDIS_MNEMONIC_HSUBPS;
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            const float* x = a.f + l * 4;
            const float* y = b.f + l * 4;
            float* o = r.f + l * 4;
            o[0] = sub ? x[0] - x[1] : x[0] + x[1];
            o[1] = sub ? x[2] - x[3] : x[2] + x[3];
            o[2] = sub ? y[0] - y[1] : y[0] + y[1];
            o[3] = sub ? y[2] - y[3] : y[2] + y[3];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_HADDPD:
    case ZYDIS_MNEMONIC_HSUBPD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const bool sub = m == ZYDIS_MNEMONIC_HSUBPD;
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            r.fd[l * 2] = sub ? a.fd[l * 2] - a.fd[l * 2 + 1] : a.fd[l * 2] + a.fd[l * 2 + 1];
            r.fd[l * 2 + 1] = sub ? b.fd[l * 2] - b.fd[l * 2 + 1] : b.fd[l * 2] + b.fd[l * 2 + 1];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_DPPS: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            float sum = 0;
            for (int i = 0; i < 4; i++) {
                if (imm & (0x10 << i)) {
                    sum += a.f[l * 4 + i] * b.f[l * 4 + i];
                }
            }
            for (int i = 0; i < 4; i++) {
                r.f[l * 4 + i] = (imm & (1 << i)) ? sum : 0.0f;
            }
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_DPPD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        double sum = 0;
        for (int i = 0; i < 2; i++) {
            if (imm & (0x10 << i)) {
                sum += a.fd[i] * b.fd[i];
            }
        }
        Vec r{};
        for (int i = 0; i < 2; i++) {
            r.fd[i] = (imm & (1 << i)) ? sum : 0.0;
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_ROUNDPS:
        UNARY_PACKED(float, f, RoundMode(x, imm, s.mxcsr));
    case ZYDIS_MNEMONIC_ROUNDPD:
        UNARY_PACKED(double, fd, RoundMode(x, imm, s.mxcsr));
    case ZYDIS_MNEMONIC_ROUNDSS:
        SCALAR(float, f, RoundMode(y, imm, s.mxcsr));
    case ZYDIS_MNEMONIC_ROUNDSD:
        SCALAR(double, fd, RoundMode(y, imm, s.mxcsr));

    // ---- logic ----
    case ZYDIS_MNEMONIC_ANDPS:
    case ZYDIS_MNEMONIC_ANDPD:
    case ZYDIS_MNEMONIC_PAND:
        PACKED(u64, q, x & y);
    case ZYDIS_MNEMONIC_ANDNPS:
    case ZYDIS_MNEMONIC_ANDNPD:
    case ZYDIS_MNEMONIC_PANDN:
        PACKED(u64, q, ~x & y);
    case ZYDIS_MNEMONIC_ORPS:
    case ZYDIS_MNEMONIC_ORPD:
    case ZYDIS_MNEMONIC_POR:
        PACKED(u64, q, x | y);
    case ZYDIS_MNEMONIC_XORPS:
    case ZYDIS_MNEMONIC_XORPD:
    case ZYDIS_MNEMONIC_PXOR:
        PACKED(u64, q, x ^ y);

    // ---- compares ----
    case ZYDIS_MNEMONIC_CMPPS:
        PACKED(u32, d, CmpPred(std::bit_cast<float>(x), std::bit_cast<float>(y), imm) ? ~0u : 0u);
    case ZYDIS_MNEMONIC_CMPPD:
        PACKED(u64, q,
               CmpPred(std::bit_cast<double>(x), std::bit_cast<double>(y), imm) ? ~0ULL : 0ULL);
    case ZYDIS_MNEMONIC_CMPSS:
        SCALAR(u32, d, CmpPred(std::bit_cast<float>(x), std::bit_cast<float>(y), imm) ? ~0u : 0u);
    case ZYDIS_MNEMONIC_CMPSD:
        if (!any_vec) {
            return false; // CMPSD the string instruction
        }
        SCALAR(u64, q,
               CmpPred(std::bit_cast<double>(x), std::bit_cast<double>(y), imm) ? ~0ULL : 0ULL);
    case ZYDIS_MNEMONIC_COMISS:
    case ZYDIS_MNEMONIC_UCOMISS:
    case ZYDIS_MNEMONIC_COMISD:
    case ZYDIS_MNEMONIC_UCOMISD: {
        const bool dbl = m == ZYDIS_MNEMONIC_COMISD || m == ZYDIS_MNEMONIC_UCOMISD;
        const Vec a = ReadVec(dst), b = ReadVec(B);
        const double x = dbl ? a.fd[0] : a.f[0], y = dbl ? b.fd[0] : b.f[0];
        u64 f = s.rflags & ~kStatusFlags;
        if (std::isnan(x) || std::isnan(y)) {
            f |= ZF | PF | CF;
        } else if (x < y) {
            f |= CF;
        } else if (x == y) {
            f |= ZF;
        }
        s.rflags = f;
        return true;
    }

    // ---- conversions ----
    case ZYDIS_MNEMONIC_CVTSI2SS:
    case ZYDIS_MNEMONIC_CVTSI2SD: {
        Vec r = ReadVec(A);
        const u64 raw = Read(B);
        const s64 v = B.size == 4 ? static_cast<s32>(raw) : static_cast<s64>(raw);
        if (m == ZYDIS_MNEMONIC_CVTSI2SS) {
            r.f[0] = static_cast<float>(v);
        } else {
            r.fd[0] = static_cast<double>(v);
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTSS2SD:
        SCALAR(double, fd, static_cast<double>(b.f[0]));
    case ZYDIS_MNEMONIC_CVTSD2SS: {
        Vec r = ReadVec(A);
        r.f[0] = static_cast<float>(ReadVec(B).fd[0]);
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTSS2SI:
    case ZYDIS_MNEMONIC_CVTTSS2SI:
    case ZYDIS_MNEMONIC_CVTSD2SI:
    case ZYDIS_MNEMONIC_CVTTSD2SI: {
        const Vec b = ReadVec(B);
        const bool t = m == ZYDIS_MNEMONIC_CVTTSS2SI || m == ZYDIS_MNEMONIC_CVTTSD2SI;
        const bool d = m == ZYDIS_MNEMONIC_CVTSD2SI || m == ZYDIS_MNEMONIC_CVTTSD2SI;
        u64 r;
        if (dst.size == 8) {
            r = static_cast<u64>(d ? CvtInt<s64>(b.fd[0], t, s.mxcsr) : CvtInt<s64>(b.f[0], t, s.mxcsr));
        } else {
            r = static_cast<u32>(d ? CvtInt<s32>(b.fd[0], t, s.mxcsr) : CvtInt<s32>(b.f[0], t, s.mxcsr));
        }
        WriteGpr(dst.reg, dst.size, r);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PS:
        UNARY_PACKED(float, f, static_cast<float>(static_cast<s32>(std::bit_cast<u32>(x))));
    case ZYDIS_MNEMONIC_CVTPS2DQ:
    case ZYDIS_MNEMONIC_CVTTPS2DQ: {
        const Vec b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 4; i++) {
            r.sd[i] = CvtInt<s32>(b.f[i], m == ZYDIS_MNEMONIC_CVTTPS2DQ, s.mxcsr);
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTDQ2PD: {
        const Vec b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 8; i++) {
            r.fd[i] = b.sd[i];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTPD2DQ:
    case ZYDIS_MNEMONIC_CVTTPD2DQ: {
        const Vec b = ReadVec(B);
        const int n = B.size >= 32 ? 4 : 2;
        Vec r{};
        for (int i = 0; i < n; i++) {
            r.sd[i] = CvtInt<s32>(b.fd[i], m == ZYDIS_MNEMONIC_CVTTPD2DQ, s.mxcsr);
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTPS2PD: {
        const Vec b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 8; i++) {
            r.fd[i] = b.f[i];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_CVTPD2PS: {
        const Vec b = ReadVec(B);
        const int n = B.size >= 32 ? 4 : 2;
        Vec r{};
        for (int i = 0; i < n; i++) {
            r.f[i] = static_cast<float>(b.fd[i]);
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_VCVTPH2PS: {
        const Vec b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 4; i++) {
            r.f[i] = HalfToFloat(b.w[i]);
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_VCVTPS2PH: {
        const Vec b = ReadVec(in.op[1]);
        const int n = in.op[1].size / 4;
        Vec r{};
        for (int i = 0; i < n; i++) {
            r.w[i] = FloatToHalf(b.f[i], imm, s.mxcsr);
        }
        WriteVec(in, dst, r, static_cast<u8>(n * 2 < 16 && dst.kind == OpKind::Reg ? 16 : n * 2));
        return true;
    }

    // ---- shuffles / blends ----
    case ZYDIS_MNEMONIC_SHUFPS: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            r.d[l * 4 + 0] = a.d[l * 4 + (imm & 3)];
            r.d[l * 4 + 1] = a.d[l * 4 + ((imm >> 2) & 3)];
            r.d[l * 4 + 2] = b.d[l * 4 + ((imm >> 4) & 3)];
            r.d[l * 4 + 3] = b.d[l * 4 + ((imm >> 6) & 3)];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_SHUFPD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            r.q[l * 2] = a.q[l * 2 + ((imm >> (l * 2)) & 1)];
            r.q[l * 2 + 1] = b.q[l * 2 + ((imm >> (l * 2 + 1)) & 1)];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_UNPCKLPS:
    case ZYDIS_MNEMONIC_UNPCKHPS:
    case ZYDIS_MNEMONIC_PUNPCKLDQ:
    case ZYDIS_MNEMONIC_PUNPCKHDQ: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const int h = (m == ZYDIS_MNEMONIC_UNPCKHPS || m == ZYDIS_MNEMONIC_PUNPCKHDQ) ? 2 : 0;
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            r.d[l * 4 + 0] = a.d[l * 4 + h];
            r.d[l * 4 + 1] = b.d[l * 4 + h];
            r.d[l * 4 + 2] = a.d[l * 4 + h + 1];
            r.d[l * 4 + 3] = b.d[l * 4 + h + 1];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_UNPCKLPD:
    case ZYDIS_MNEMONIC_UNPCKHPD:
    case ZYDIS_MNEMONIC_PUNPCKLQDQ:
    case ZYDIS_MNEMONIC_PUNPCKHQDQ: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const int h = (m == ZYDIS_MNEMONIC_UNPCKHPD || m == ZYDIS_MNEMONIC_PUNPCKHQDQ) ? 1 : 0;
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            r.q[l * 2] = a.q[l * 2 + h];
            r.q[l * 2 + 1] = b.q[l * 2 + h];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_PUNPCKLBW:
    case ZYDIS_MNEMONIC_PUNPCKHBW: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const int h = m == ZYDIS_MNEMONIC_PUNPCKHBW ? 8 : 0;
        Vec r{};
        for (int i = 0; i < 8; i++) {
            r.b[i * 2] = a.b[h + i];
            r.b[i * 2 + 1] = b.b[h + i];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PUNPCKLWD:
    case ZYDIS_MNEMONIC_PUNPCKHWD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const int h = m == ZYDIS_MNEMONIC_PUNPCKHWD ? 4 : 0;
        Vec r{};
        for (int i = 0; i < 4; i++) {
            r.w[i * 2] = a.w[h + i];
            r.w[i * 2 + 1] = b.w[h + i];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PSHUFD: {
        const Vec b = ReadVec(B);
        Vec r{};
        for (int l = 0; l < vl / 16; l++) {
            for (int i = 0; i < 4; i++) {
                r.d[l * 4 + i] = b.d[l * 4 + ((imm >> (i * 2)) & 3)];
            }
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_PSHUFHW:
    case ZYDIS_MNEMONIC_PSHUFLW: {
        const Vec b = ReadVec(B);
        Vec r = b;
        const int base = m == ZYDIS_MNEMONIC_PSHUFHW ? 4 : 0;
        for (int i = 0; i < 4; i++) {
            r.w[base + i] = b.w[base + ((imm >> (i * 2)) & 3)];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PSHUFB: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < 16; i++) {
            r.b[i] = (b.b[i] & 0x80) ? 0 : a.b[b.b[i] & 15];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PALIGNR: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        u8 cat[32];
        std::memcpy(cat, b.b, 16);
        std::memcpy(cat + 16, a.b, 16);
        Vec r{};
        for (int i = 0; i < 16; i++) {
            r.b[i] = (imm + i) < 32 ? cat[imm + i] : 0;
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_BLENDPS: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 4; i++) {
            r.d[i] = (imm >> i) & 1 ? b.d[i] : a.d[i];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_BLENDPD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < vl / 8; i++) {
            r.q[i] = (imm >> i) & 1 ? b.q[i] : a.q[i];
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_PBLENDW: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < 8; i++) {
            r.w[i] = (imm >> i) & 1 ? b.w[i] : a.w[i];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_BLENDVPS:
    case ZYDIS_MNEMONIC_BLENDVPD:
    case ZYDIS_MNEMONIC_PBLENDVB: {
        // Legacy: mask is implicit xmm0 (Zydis may list it as a visible third operand).
        // VEX: four operands, the last being the mask register.
        const Op& sa = vex ? in.op[1] : in.op[0];
        const Op& sb = vex ? in.op[2] : in.op[1];
        const Vec a = ReadVec(sa), b = ReadVec(sb);
        const Vec mask = vex ? ReadVec(in.op[3]) : s.ymm[0];
        const int w = m == ZYDIS_MNEMONIC_BLENDVPS ? 4 : m == ZYDIS_MNEMONIC_BLENDVPD ? 8 : 1;
        Vec r{};
        for (int i = 0; i < vl / w; i++) {
            const bool sel = mask.b[i * w + w - 1] & 0x80;
            std::memcpy(r.b + i * w, (sel ? b.b : a.b) + i * w, w);
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_INSERTPS: {
        Vec r = ReadVec(A);
        u32 v;
        if (B.kind == OpKind::Mem) {
            v = Load<u32>(EffectiveAddress(B));
        } else {
            v = ReadVec(B).d[(imm >> 6) & 3];
        }
        r.d[(imm >> 4) & 3] = v;
        for (int i = 0; i < 4; i++) {
            if (imm & (1 << i)) {
                r.d[i] = 0;
            }
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_EXTRACTPS: {
        const u32 v = ReadVec(in.op[1]).d[imm & 3];
        if (dst.kind == OpKind::Mem) {
            Store<u32>(EffectiveAddress(dst), v);
        } else {
            WriteGpr(dst.reg, dst.size, v);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_PINSRB:
    case ZYDIS_MNEMONIC_PINSRW:
    case ZYDIS_MNEMONIC_PINSRD:
    case ZYDIS_MNEMONIC_PINSRQ: {
        Vec r = ReadVec(A);
        const u64 v = Read(B);
        switch (m) {
        case ZYDIS_MNEMONIC_PINSRB:
            r.b[imm & 15] = static_cast<u8>(v);
            break;
        case ZYDIS_MNEMONIC_PINSRW:
            r.w[imm & 7] = static_cast<u16>(v);
            break;
        case ZYDIS_MNEMONIC_PINSRD:
            r.d[imm & 3] = static_cast<u32>(v);
            break;
        default:
            r.q[imm & 1] = v;
            break;
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PEXTRB:
    case ZYDIS_MNEMONIC_PEXTRW:
    case ZYDIS_MNEMONIC_PEXTRD:
    case ZYDIS_MNEMONIC_PEXTRQ: {
        const Vec v = ReadVec(in.op[1]);
        u64 r;
        u8 w;
        switch (m) {
        case ZYDIS_MNEMONIC_PEXTRB:
            r = v.b[imm & 15], w = 1;
            break;
        case ZYDIS_MNEMONIC_PEXTRW:
            r = v.w[imm & 7], w = 2;
            break;
        case ZYDIS_MNEMONIC_PEXTRD:
            r = v.d[imm & 3], w = 4;
            break;
        default:
            r = v.q[imm & 1], w = 8;
            break;
        }
        if (dst.kind == OpKind::Mem) {
            std::memcpy(Ptr(EffectiveAddress(dst), w), &r, w);
        } else {
            WriteGpr(dst.reg, dst.size, r);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_PMOVZXBW:
    case ZYDIS_MNEMONIC_PMOVZXBD:
    case ZYDIS_MNEMONIC_PMOVZXBQ:
    case ZYDIS_MNEMONIC_PMOVZXWD:
    case ZYDIS_MNEMONIC_PMOVZXWQ:
    case ZYDIS_MNEMONIC_PMOVZXDQ:
    case ZYDIS_MNEMONIC_PMOVSXBW:
    case ZYDIS_MNEMONIC_PMOVSXBD:
    case ZYDIS_MNEMONIC_PMOVSXBQ:
    case ZYDIS_MNEMONIC_PMOVSXWD:
    case ZYDIS_MNEMONIC_PMOVSXWQ:
    case ZYDIS_MNEMONIC_PMOVSXDQ: {
        const Vec b = ReadVec(B);
        int from, to;
        bool sx;
        switch (m) {
        case ZYDIS_MNEMONIC_PMOVZXBW: from = 1, to = 2, sx = false; break;
        case ZYDIS_MNEMONIC_PMOVZXBD: from = 1, to = 4, sx = false; break;
        case ZYDIS_MNEMONIC_PMOVZXBQ: from = 1, to = 8, sx = false; break;
        case ZYDIS_MNEMONIC_PMOVZXWD: from = 2, to = 4, sx = false; break;
        case ZYDIS_MNEMONIC_PMOVZXWQ: from = 2, to = 8, sx = false; break;
        case ZYDIS_MNEMONIC_PMOVZXDQ: from = 4, to = 8, sx = false; break;
        case ZYDIS_MNEMONIC_PMOVSXBW: from = 1, to = 2, sx = true; break;
        case ZYDIS_MNEMONIC_PMOVSXBD: from = 1, to = 4, sx = true; break;
        case ZYDIS_MNEMONIC_PMOVSXBQ: from = 1, to = 8, sx = true; break;
        case ZYDIS_MNEMONIC_PMOVSXWD: from = 2, to = 4, sx = true; break;
        case ZYDIS_MNEMONIC_PMOVSXWQ: from = 2, to = 8, sx = true; break;
        default: from = 4, to = 8, sx = true; break;
        }
        Vec r{};
        for (int i = 0; i < 16 / to; i++) {
            u64 v = 0;
            std::memcpy(&v, b.b + i * from, from);
            if (sx) {
                const int sh = 64 - from * 8;
                v = static_cast<u64>(static_cast<s64>(v << sh) >> sh);
            }
            std::memcpy(r.b + i * to, &v, to);
        }
        out(r, 16);
        return true;
    }

    // ---- packed integer arithmetic ----
    case ZYDIS_MNEMONIC_PADDB:
        PACKED(u8, b, x + y);
    case ZYDIS_MNEMONIC_PADDW:
        PACKED(u16, w, x + y);
    case ZYDIS_MNEMONIC_PADDD:
        PACKED(u32, d, x + y);
    case ZYDIS_MNEMONIC_PADDQ:
        PACKED(u64, q, x + y);
    case ZYDIS_MNEMONIC_PSUBB:
        PACKED(u8, b, x - y);
    case ZYDIS_MNEMONIC_PSUBW:
        PACKED(u16, w, x - y);
    case ZYDIS_MNEMONIC_PSUBD:
        PACKED(u32, d, x - y);
    case ZYDIS_MNEMONIC_PSUBQ:
        PACKED(u64, q, x - y);
    case ZYDIS_MNEMONIC_PADDSB:
        PACKED(s8, sb, SatS<s8>(s64{x} + y));
    case ZYDIS_MNEMONIC_PADDSW:
        PACKED(s16, sw, SatS<s16>(s64{x} + y));
    case ZYDIS_MNEMONIC_PADDUSB:
        PACKED(u8, b, SatU<u8>(s64{x} + y));
    case ZYDIS_MNEMONIC_PADDUSW:
        PACKED(u16, w, SatU<u16>(s64{x} + y));
    case ZYDIS_MNEMONIC_PSUBSB:
        PACKED(s8, sb, SatS<s8>(s64{x} - y));
    case ZYDIS_MNEMONIC_PSUBSW:
        PACKED(s16, sw, SatS<s16>(s64{x} - y));
    case ZYDIS_MNEMONIC_PSUBUSB:
        PACKED(u8, b, SatU<u8>(s64{x} - y));
    case ZYDIS_MNEMONIC_PSUBUSW:
        PACKED(u16, w, SatU<u16>(s64{x} - y));
    case ZYDIS_MNEMONIC_PMULLW:
        PACKED(u16, w, x * y);
    case ZYDIS_MNEMONIC_PMULLD:
        PACKED(u32, d, x * y);
    case ZYDIS_MNEMONIC_PMULHW:
        PACKED(s16, sw, (s32{x} * y) >> 16);
    case ZYDIS_MNEMONIC_PMULHUW:
        PACKED(u16, w, (u32{x} * y) >> 16);
    case ZYDIS_MNEMONIC_PMULHRSW:
        PACKED(s16, sw, ((s32{x} * y >> 14) + 1) >> 1);
    case ZYDIS_MNEMONIC_PMULUDQ: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < 2; i++) {
            r.q[i] = static_cast<u64>(a.d[i * 2]) * b.d[i * 2];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PMULDQ: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < 2; i++) {
            r.sq[i] = static_cast<s64>(a.sd[i * 2]) * b.sd[i * 2];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PMADDWD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < 4; i++) {
            r.sd[i] = s32{a.sw[i * 2]} * b.sw[i * 2] + s32{a.sw[i * 2 + 1]} * b.sw[i * 2 + 1];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PMADDUBSW: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int i = 0; i < 8; i++) {
            r.sw[i] = SatS<s16>(s32{a.b[i * 2]} * b.sb[i * 2] + s32{a.b[i * 2 + 1]} * b.sb[i * 2 + 1]);
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PAVGB:
        PACKED(u8, b, (u32{x} + y + 1) >> 1);
    case ZYDIS_MNEMONIC_PAVGW:
        PACKED(u16, w, (u32{x} + y + 1) >> 1);
    case ZYDIS_MNEMONIC_PSADBW: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        Vec r{};
        for (int h = 0; h < 2; h++) {
            u32 sum = 0;
            for (int i = 0; i < 8; i++) {
                sum += static_cast<u32>(std::abs(int{a.b[h * 8 + i]} - int{b.b[h * 8 + i]}));
            }
            r.q[h] = sum;
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PMINUB:
        PACKED(u8, b, std::min(x, y));
    case ZYDIS_MNEMONIC_PMINSB:
        PACKED(s8, sb, std::min(x, y));
    case ZYDIS_MNEMONIC_PMINUW:
        PACKED(u16, w, std::min(x, y));
    case ZYDIS_MNEMONIC_PMINSW:
        PACKED(s16, sw, std::min(x, y));
    case ZYDIS_MNEMONIC_PMINUD:
        PACKED(u32, d, std::min(x, y));
    case ZYDIS_MNEMONIC_PMINSD:
        PACKED(s32, sd, std::min(x, y));
    case ZYDIS_MNEMONIC_PMAXUB:
        PACKED(u8, b, std::max(x, y));
    case ZYDIS_MNEMONIC_PMAXSB:
        PACKED(s8, sb, std::max(x, y));
    case ZYDIS_MNEMONIC_PMAXUW:
        PACKED(u16, w, std::max(x, y));
    case ZYDIS_MNEMONIC_PMAXSW:
        PACKED(s16, sw, std::max(x, y));
    case ZYDIS_MNEMONIC_PMAXUD:
        PACKED(u32, d, std::max(x, y));
    case ZYDIS_MNEMONIC_PMAXSD:
        PACKED(s32, sd, std::max(x, y));
    case ZYDIS_MNEMONIC_PABSB:
        UNARY_PACKED(s8, sb, x < 0 ? -x : x);
    case ZYDIS_MNEMONIC_PABSW:
        UNARY_PACKED(s16, sw, x < 0 ? -x : x);
    case ZYDIS_MNEMONIC_PABSD:
        UNARY_PACKED(s32, sd, x < 0 ? -static_cast<s64>(x) : x);
    case ZYDIS_MNEMONIC_PSIGNB:
        PACKED(s8, sb, y < 0 ? -x : y == 0 ? 0 : x);
    case ZYDIS_MNEMONIC_PSIGNW:
        PACKED(s16, sw, y < 0 ? -x : y == 0 ? 0 : x);
    case ZYDIS_MNEMONIC_PSIGND:
        PACKED(s32, sd, y < 0 ? -static_cast<s64>(x) : y == 0 ? 0 : x);
    case ZYDIS_MNEMONIC_PHADDW:
    case ZYDIS_MNEMONIC_PHSUBW:
    case ZYDIS_MNEMONIC_PHADDSW:
    case ZYDIS_MNEMONIC_PHSUBSW: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const bool sub = m == ZYDIS_MNEMONIC_PHSUBW || m == ZYDIS_MNEMONIC_PHSUBSW;
        const bool sat = m == ZYDIS_MNEMONIC_PHADDSW || m == ZYDIS_MNEMONIC_PHSUBSW;
        Vec r{};
        for (int i = 0; i < 8; i++) {
            const Vec& srcv = i < 4 ? a : b;
            const int j = (i & 3) * 2;
            const s32 v = sub ? s32{srcv.sw[j]} - srcv.sw[j + 1] : s32{srcv.sw[j]} + srcv.sw[j + 1];
            r.sw[i] = sat ? SatS<s16>(v) : static_cast<s16>(v);
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PHADDD:
    case ZYDIS_MNEMONIC_PHSUBD: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const bool sub = m == ZYDIS_MNEMONIC_PHSUBD;
        Vec r{};
        for (int i = 0; i < 4; i++) {
            const Vec& srcv = i < 2 ? a : b;
            const int j = (i & 1) * 2;
            r.d[i] = sub ? srcv.d[j] - srcv.d[j + 1] : srcv.d[j] + srcv.d[j + 1];
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PHMINPOSUW: {
        const Vec b = ReadVec(B);
        int idx = 0;
        for (int i = 1; i < 8; i++) {
            if (b.w[i] < b.w[idx]) {
                idx = i;
            }
        }
        Vec r{};
        r.w[0] = b.w[idx];
        r.w[1] = static_cast<u16>(idx);
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PCMPEQB:
        PACKED(u8, b, x == y ? 0xFF : 0);
    case ZYDIS_MNEMONIC_PCMPEQW:
        PACKED(u16, w, x == y ? 0xFFFF : 0);
    case ZYDIS_MNEMONIC_PCMPEQD:
        PACKED(u32, d, x == y ? ~0u : 0u);
    case ZYDIS_MNEMONIC_PCMPEQQ:
        PACKED(u64, q, x == y ? ~0ULL : 0ULL);
    case ZYDIS_MNEMONIC_PCMPGTB:
        PACKED(s8, sb, x > y ? -1 : 0);
    case ZYDIS_MNEMONIC_PCMPGTW:
        PACKED(s16, sw, x > y ? -1 : 0);
    case ZYDIS_MNEMONIC_PCMPGTD:
        PACKED(s32, sd, x > y ? -1 : 0);
    case ZYDIS_MNEMONIC_PCMPGTQ:
        PACKED(s64, sq, x > y ? -1 : 0);
    case ZYDIS_MNEMONIC_PACKSSWB:
    case ZYDIS_MNEMONIC_PACKUSWB: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const bool u = m == ZYDIS_MNEMONIC_PACKUSWB;
        Vec r{};
        for (int i = 0; i < 8; i++) {
            r.b[i] = u ? SatU<u8>(a.sw[i]) : static_cast<u8>(SatS<s8>(a.sw[i]));
            r.b[i + 8] = u ? SatU<u8>(b.sw[i]) : static_cast<u8>(SatS<s8>(b.sw[i]));
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PACKSSDW:
    case ZYDIS_MNEMONIC_PACKUSDW: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const bool u = m == ZYDIS_MNEMONIC_PACKUSDW;
        Vec r{};
        for (int i = 0; i < 4; i++) {
            r.w[i] = u ? SatU<u16>(a.sd[i]) : static_cast<u16>(SatS<s16>(a.sd[i]));
            r.w[i + 4] = u ? SatU<u16>(b.sd[i]) : static_cast<u16>(SatS<s16>(b.sd[i]));
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PTEST:
    case ZYDIS_MNEMONIC_VTESTPS:
    case ZYDIS_MNEMONIC_VTESTPD: {
        const Vec a = ReadVec(dst), b = ReadVec(B);
        bool zf = true, cf = true;
        if (m == ZYDIS_MNEMONIC_PTEST) {
            for (int i = 0; i < vl / 8; i++) {
                zf &= (a.q[i] & b.q[i]) == 0;
                cf &= (~a.q[i] & b.q[i]) == 0;
            }
        } else {
            const int w = m == ZYDIS_MNEMONIC_VTESTPS ? 4 : 8;
            for (int i = 0; i < vl / w; i++) {
                const bool sa = a.b[i * w + w - 1] & 0x80, sb = b.b[i * w + w - 1] & 0x80;
                zf &= !(sa && sb);
                cf &= !(!sa && sb);
            }
        }
        s.rflags = (s.rflags & ~kStatusFlags) | (zf ? ZF : 0) | (cf ? CF : 0);
        return true;
    }

    // ---- shifts ----
    case ZYDIS_MNEMONIC_PSLLW:
    case ZYDIS_MNEMONIC_PSLLD:
    case ZYDIS_MNEMONIC_PSLLQ:
    case ZYDIS_MNEMONIC_PSRLW:
    case ZYDIS_MNEMONIC_PSRLD:
    case ZYDIS_MNEMONIC_PSRLQ:
    case ZYDIS_MNEMONIC_PSRAW:
    case ZYDIS_MNEMONIC_PSRAD: {
        const bool by_imm = imm_op != nullptr;
        const Vec v = ReadVec(by_imm ? B : A);
        const u64 count = by_imm ? static_cast<u64>(imm) : ReadVec(B).q[0];
        const int w = (m == ZYDIS_MNEMONIC_PSLLW || m == ZYDIS_MNEMONIC_PSRLW || m == ZYDIS_MNEMONIC_PSRAW) ? 2
                      : (m == ZYDIS_MNEMONIC_PSLLD || m == ZYDIS_MNEMONIC_PSRLD || m == ZYDIS_MNEMONIC_PSRAD) ? 4
                                                                                                              : 8;
        const bool left = m == ZYDIS_MNEMONIC_PSLLW || m == ZYDIS_MNEMONIC_PSLLD || m == ZYDIS_MNEMONIC_PSLLQ;
        const bool arith = m == ZYDIS_MNEMONIC_PSRAW || m == ZYDIS_MNEMONIC_PSRAD;
        const u64 bits = static_cast<u64>(w) * 8;
        Vec r{};
        for (int i = 0; i < 16 / w; i++) {
            u64 x = 0;
            std::memcpy(&x, v.b + i * w, w);
            u64 y;
            if (arith) {
                const int sh = 64 - static_cast<int>(bits);
                const s64 sx = static_cast<s64>(x << sh) >> sh;
                y = static_cast<u64>(sx >> std::min<u64>(count, bits - 1));
            } else if (count >= bits) {
                y = 0;
            } else {
                y = left ? (x << count) : (x >> count);
            }
            std::memcpy(r.b + i * w, &y, w);
        }
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PSLLDQ:
    case ZYDIS_MNEMONIC_PSRLDQ: {
        const Vec v = ReadVec(B);
        Vec r{};
        const int n = std::min(imm, 16);
        for (int i = 0; i < 16; i++) {
            if (m == ZYDIS_MNEMONIC_PSLLDQ) {
                r.b[i] = i >= n ? v.b[i - n] : 0;
            } else {
                r.b[i] = i + n < 16 ? v.b[i + n] : 0;
            }
        }
        out(r, 16);
        return true;
    }

    // ---- AVX-only ----
    case ZYDIS_MNEMONIC_VZEROUPPER:
        for (auto& y : s.ymm) {
            y.q[2] = y.q[3] = 0;
        }
        return true;
    case ZYDIS_MNEMONIC_VZEROALL:
        for (auto& y : s.ymm) {
            y = {};
        }
        return true;
    case ZYDIS_MNEMONIC_VBROADCASTSS: {
        const u32 v = ReadVec(B).d[0];
        Vec r{};
        for (int i = 0; i < vl / 4; i++) {
            r.d[i] = v;
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_VBROADCASTSD: {
        const u64 v = ReadVec(B).q[0];
        Vec r{};
        for (int i = 0; i < 4; i++) {
            r.q[i] = v;
        }
        out(r, 32);
        return true;
    }
    case ZYDIS_MNEMONIC_VBROADCASTF128: {
        const Vec v = ReadVec(B);
        Vec r{};
        r.q[0] = r.q[2] = v.q[0];
        r.q[1] = r.q[3] = v.q[1];
        out(r, 32);
        return true;
    }
    case ZYDIS_MNEMONIC_VINSERTF128: {
        Vec r = ReadVec(A);
        if (A.size <= 16) {
            r = s.ymm[A.reg - kRegVec];
        }
        const Vec b = ReadVec(B);
        std::memcpy(r.b + (imm & 1) * 16, b.b, 16);
        out(r, 32);
        return true;
    }
    case ZYDIS_MNEMONIC_VEXTRACTF128: {
        const Vec v = s.ymm[in.op[1].reg - kRegVec];
        Vec r{};
        std::memcpy(r.b, v.b + (imm & 1) * 16, 16);
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_VPERM2F128: {
        const Vec a = s.ymm[A.reg - kRegVec];
        const Vec b = B.kind == OpKind::Mem ? ReadVec(B) : s.ymm[B.reg - kRegVec];
        Vec r{};
        for (int h = 0; h < 2; h++) {
            const int sel = (imm >> (h * 4)) & 0xF;
            if (sel & 8) {
                continue;
            }
            const Vec& srcv = (sel & 2) ? b : a;
            std::memcpy(r.b + h * 16, srcv.b + (sel & 1) * 16, 16);
        }
        out(r, 32);
        return true;
    }
    case ZYDIS_MNEMONIC_VPERMILPS: {
        Vec r{};
        if (imm_op) {
            const Vec v = ReadVec(B);
            for (int l = 0; l < vl / 16; l++) {
                for (int i = 0; i < 4; i++) {
                    r.d[l * 4 + i] = v.d[l * 4 + ((imm >> (i * 2)) & 3)];
                }
            }
        } else {
            const Vec v = ReadVec(A), c = ReadVec(B);
            for (int i = 0; i < vl / 4; i++) {
                r.d[i] = v.d[(i & ~3) + (c.d[i] & 3)];
            }
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_VPERMILPD: {
        Vec r{};
        if (imm_op) {
            const Vec v = ReadVec(B);
            for (int i = 0; i < vl / 8; i++) {
                r.q[i] = v.q[(i & ~1) + ((imm >> i) & 1)];
            }
        } else {
            const Vec v = ReadVec(A), c = ReadVec(B);
            for (int i = 0; i < vl / 8; i++) {
                r.q[i] = v.q[(i & ~1) + ((c.q[i] >> 1) & 1)];
            }
        }
        out(r, vl);
        return true;
    }
    case ZYDIS_MNEMONIC_VMASKMOVPS:
    case ZYDIS_MNEMONIC_VMASKMOVPD: {
        const int w = m == ZYDIS_MNEMONIC_VMASKMOVPS ? 4 : 8;
        if (dst.kind == OpKind::Mem) {
            const Vec mask = ReadVec(in.op[1]), v = ReadVec(in.op[2]);
            const u64 base = EffectiveAddress(dst);
            for (int i = 0; i < vl / w; i++) {
                if (mask.b[i * w + w - 1] & 0x80) {
                    std::memcpy(Ptr(base + i * w, w), v.b + i * w, w);
                }
            }
        } else {
            const Vec mask = ReadVec(in.op[1]);
            const u64 base = EffectiveAddress(in.op[2]);
            Vec r{};
            for (int i = 0; i < vl / w; i++) {
                if (mask.b[i * w + w - 1] & 0x80) {
                    std::memcpy(r.b + i * w, Ptr(base + i * w, w), w);
                }
            }
            out(r, vl);
        }
        return true;
    }

    // ---- state ----
    case ZYDIS_MNEMONIC_LDMXCSR:
        s.mxcsr = Load<u32>(EffectiveAddress(dst));
        return true;
    case ZYDIS_MNEMONIC_STMXCSR:
        Store<u32>(EffectiveAddress(dst), s.mxcsr);
        return true;
    case ZYDIS_MNEMONIC_FXSAVE:
    case ZYDIS_MNEMONIC_FXSAVE64:
    case ZYDIS_MNEMONIC_XSAVE:
    case ZYDIS_MNEMONIC_XSAVE64:
    case ZYDIS_MNEMONIC_XSAVEOPT:
    case ZYDIS_MNEMONIC_XSAVEOPT64:
    case ZYDIS_MNEMONIC_XSAVEC:
    case ZYDIS_MNEMONIC_XSAVEC64: {
        const bool fx = m == ZYDIS_MNEMONIC_FXSAVE || m == ZYDIS_MNEMONIC_FXSAVE64;
        u8* p = Ptr(EffectiveAddress(dst), fx ? 512 : 832);
        std::memset(p, 0, 512);
        const u16 sw = (s.fpu_sw & ~0x3800) | ((s.fpu_top & 7) << 11);
        std::memcpy(p + 0, &s.fpu_cw, 2);
        std::memcpy(p + 2, &sw, 2);
        p[4] = static_cast<u8>(~s.fpu_tag_empty);
        std::memcpy(p + 24, &s.mxcsr, 4);
        const u32 mask = 0xFFBF;
        std::memcpy(p + 28, &mask, 4);
        for (int i = 0; i < 8; i++) {
            std::memcpy(p + 32 + i * 16, &s.st[(s.fpu_top + i) & 7], 8);
        }
        for (int i = 0; i < 16; i++) {
            std::memcpy(p + 160 + i * 16, s.ymm[i].b, 16);
        }
        if (!fx) {
            const u64 xstate_bv = 7;
            std::memcpy(p + 512, &xstate_bv, 8);
            std::memset(p + 520, 0, 56);
            for (int i = 0; i < 16; i++) {
                std::memcpy(p + 576 + i * 16, s.ymm[i].b + 16, 16);
            }
        }
        return true;
    }
    case ZYDIS_MNEMONIC_FXRSTOR:
    case ZYDIS_MNEMONIC_FXRSTOR64:
    case ZYDIS_MNEMONIC_XRSTOR:
    case ZYDIS_MNEMONIC_XRSTOR64: {
        const bool fx = m == ZYDIS_MNEMONIC_FXRSTOR || m == ZYDIS_MNEMONIC_FXRSTOR64;
        const u8* p = Ptr(EffectiveAddress(dst), fx ? 512 : 832);
        u16 sw;
        std::memcpy(&s.fpu_cw, p + 0, 2);
        std::memcpy(&sw, p + 2, 2);
        s.fpu_sw = sw & ~0x3800;
        s.fpu_top = (sw >> 11) & 7;
        s.fpu_tag_empty = static_cast<u8>(~p[4]);
        std::memcpy(&s.mxcsr, p + 24, 4);
        for (int i = 0; i < 8; i++) {
            std::memcpy(&s.st[(s.fpu_top + i) & 7], p + 32 + i * 16, 8);
        }
        for (int i = 0; i < 16; i++) {
            std::memcpy(s.ymm[i].b, p + 160 + i * 16, 16);
        }
        if (!fx) {
            for (int i = 0; i < 16; i++) {
                std::memcpy(s.ymm[i].b + 16, p + 576 + i * 16, 16);
            }
        }
        return true;
    }
    case ZYDIS_MNEMONIC_EMMS:
        s.fpu_tag_empty = 0xFF;
        return true;

    // ---- SSE4.2 / crypto ----
    case ZYDIS_MNEMONIC_CRC32: {
        u32 crc = static_cast<u32>(ReadGpr(dst.reg, 4));
        const u64 v = Read(in.op[1]);
        for (int i = 0; i < in.op[1].size; i++) {
            crc = Crc32cByte(crc, static_cast<u8>(v >> (i * 8)));
        }
        WriteGpr(dst.reg, dst.size, crc);
        return true;
    }
    case ZYDIS_MNEMONIC_AESENC:
    case ZYDIS_MNEMONIC_AESENCLAST:
    case ZYDIS_MNEMONIC_AESDEC:
    case ZYDIS_MNEMONIC_AESDECLAST: {
        Vec st = ReadVec(A);
        const Vec key = ReadVec(B);
        const bool dec = m == ZYDIS_MNEMONIC_AESDEC || m == ZYDIS_MNEMONIC_AESDECLAST;
        const bool last = m == ZYDIS_MNEMONIC_AESENCLAST || m == ZYDIS_MNEMONIC_AESDECLAST;
        ShiftRows(st.b, dec);
        for (int i = 0; i < 16; i++) {
            st.b[i] = dec ? InvSbox(st.b[i]) : kSbox[st.b[i]];
        }
        if (!last) {
            MixColumns(st.b, dec);
        }
        for (int i = 0; i < 16; i++) {
            st.b[i] ^= key.b[i];
        }
        std::memset(st.b + 16, 0, 16);
        out(st, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_AESIMC: {
        Vec st = ReadVec(B);
        MixColumns(st.b, true);
        out(st, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_AESKEYGENASSIST: {
        const Vec v = ReadVec(B);
        const auto sub = [](u32 w) {
            return u32{kSbox[w & 0xFF]} | (u32{kSbox[(w >> 8) & 0xFF]} << 8) |
                   (u32{kSbox[(w >> 16) & 0xFF]} << 16) | (u32{kSbox[w >> 24]} << 24);
        };
        const u32 x1 = sub(v.d[1]), x3 = sub(v.d[3]);
        Vec r{};
        r.d[0] = x1;
        r.d[1] = std::rotr(x1, 8) ^ static_cast<u32>(imm);
        r.d[2] = x3;
        r.d[3] = std::rotr(x3, 8) ^ static_cast<u32>(imm);
        out(r, 16);
        return true;
    }
    case ZYDIS_MNEMONIC_PCLMULQDQ: {
        const Vec a = ReadVec(A), b = ReadVec(B);
        const u64 x = a.q[imm & 1], y = b.q[(imm >> 4) & 1];
        u64 lo = 0, hi = 0;
        for (int i = 0; i < 64; i++) {
            if ((y >> i) & 1) {
                lo ^= x << i;
                hi ^= i ? (x >> (64 - i)) : 0;
            }
        }
        Vec r{};
        r.q[0] = lo;
        r.q[1] = hi;
        out(r, 16);
        return true;
    }
    default:
        return false;
    }
#undef PACKED
#undef SCALAR
#undef UNARY_PACKED
}

} // namespace Core::Interpreter
