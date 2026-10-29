// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// x87 floating point, computed in double precision. 80-bit memory operands are converted
// to and from the extended format exactly; arithmetic loses the extra 11 bits of mantissa, which
// is acceptable for the PS4 SDK's code (it is SSE-based; x87 appears in libm paths and control
// word setup).

#include <bit>
#include <cmath>
#include "core/interpreter/cpu.h"

namespace Core::Interpreter {

namespace {

double FromExtended(const u8* p) {
    u64 mant;
    u16 se;
    std::memcpy(&mant, p, 8);
    std::memcpy(&se, p + 8, 2);
    const bool neg = se & 0x8000;
    const int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) {
        v = 0.0;
    } else if (exp == 0x7FFF) {
        v = (mant << 1) ? std::nan("") : INFINITY;
    } else {
        v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
    }
    return neg ? -v : v;
}

void ToExtended(double v, u8* p) {
    u64 mant = 0;
    u16 se = std::signbit(v) ? 0x8000 : 0;
    if (std::isnan(v)) {
        se |= 0x7FFF;
        mant = 0xC000000000000000ULL;
    } else if (std::isinf(v)) {
        se |= 0x7FFF;
        mant = 0x8000000000000000ULL;
    } else if (v != 0.0) {
        int e;
        const double m = std::frexp(std::fabs(v), &e); // m in [0.5, 1)
        mant = static_cast<u64>(std::ldexp(m, 64));
        se |= static_cast<u16>(e - 1 + 16383);
    }
    std::memcpy(p, &mant, 8);
    std::memcpy(p + 8, &se, 2);
}

double RoundByControl(double v, u16 cw) {
    switch ((cw >> 10) & 3) {
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

} // namespace

bool Cpu::ExecX87(const Insn& in) {
    auto& st = s.st;
    const auto phys = [&](int i) { return (s.fpu_top + i) & 7; };
    const auto ST = [&](int i) -> double& { return st[phys(i)]; };
    const auto push = [&](double v) {
        s.fpu_top = (s.fpu_top - 1) & 7;
        st[s.fpu_top] = v;
        s.fpu_tag_empty &= ~(1u << s.fpu_top);
    };
    const auto pop = [&]() {
        s.fpu_tag_empty |= 1u << s.fpu_top;
        s.fpu_top = (s.fpu_top + 1) & 7;
    };
    const auto set_cc = [&](bool c0, bool c2, bool c3) {
        s.fpu_sw = (s.fpu_sw & ~0x4500) | (c0 ? 0x100 : 0) | (c2 ? 0x400 : 0) | (c3 ? 0x4000 : 0);
    };
    // Locates the memory operand / ST(i) operands among the visible ones.
    const Op* mem = nullptr;
    int st_idx[2] = {-1, -1};
    int nst = 0;
    for (u8 i = 0; i < in.nops; i++) {
        if (in.op[i].kind == OpKind::Mem) {
            mem = &in.op[i];
        } else if (in.op[i].kind == OpKind::Reg && in.op[i].reg >= kRegSt &&
                   in.op[i].reg < kRegSt + 8 && nst < 2) {
            st_idx[nst++] = in.op[i].reg - kRegSt;
        }
    }
    const auto load_fp = [&](const Op& o) -> double {
        const u64 a = EffectiveAddress(o);
        switch (o.size) {
        case 4:
            return Load<float>(a);
        case 8:
            return Load<double>(a);
        default:
            return FromExtended(Ptr(a, 10));
        }
    };
    const auto load_int = [&](const Op& o) -> double {
        const u64 a = EffectiveAddress(o);
        switch (o.size) {
        case 2:
            return Load<s16>(a);
        case 4:
            return Load<s32>(a);
        default:
            return static_cast<double>(Load<s64>(a));
        }
    };
    const auto store_int = [&](const Op& o, double v) {
        const u64 a = EffectiveAddress(o);
        const double lim = o.size == 2 ? 32768.0 : o.size == 4 ? 2147483648.0 : 9223372036854775808.0;
        const bool bad = std::isnan(v) || v >= lim || v < -lim;
        switch (o.size) {
        case 2:
            Store<s16>(a, bad ? INT16_MIN : static_cast<s16>(v));
            break;
        case 4:
            Store<s32>(a, bad ? INT32_MIN : static_cast<s32>(v));
            break;
        default:
            Store<s64>(a, bad ? INT64_MIN : static_cast<s64>(v));
            break;
        }
    };
    // Binary arithmetic: memory form ST0 = ST0 op m; register form dst = dst op src.
    const auto arith = [&](auto op, bool reverse, bool pop_after, bool int_mem) {
        if (mem) {
            const double m = int_mem ? load_int(*mem) : load_fp(*mem);
            ST(0) = reverse ? op(m, ST(0)) : op(ST(0), m);
            return;
        }
        int d = nst > 0 ? st_idx[0] : 1;
        int sidx = nst > 1 ? st_idx[1] : 0;
        ST(d) = reverse ? op(ST(sidx), ST(d)) : op(ST(d), ST(sidx));
        if (pop_after) {
            pop();
        }
    };
    const auto compare = [&](double a, double b, bool to_eflags) {
        const bool un = std::isnan(a) || std::isnan(b);
        const bool lt = !un && a < b, eq = !un && a == b;
        if (to_eflags) {
            s.rflags = (s.rflags & ~(ZF | PF | CF | OF | SF | AF)) | ((un || eq) ? ZF : 0) |
                       (un ? PF : 0) | ((un || lt) ? CF : 0);
        } else {
            set_cc(un || lt, un, un || eq);
        }
    };

    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_FLD:
        if (mem) {
            push(load_fp(*mem));
        } else {
            push(ST(st_idx[0]));
        }
        return true;
    case ZYDIS_MNEMONIC_FILD:
        push(load_int(*mem));
        return true;
    case ZYDIS_MNEMONIC_FST:
    case ZYDIS_MNEMONIC_FSTP: {
        if (mem) {
            const u64 a = EffectiveAddress(*mem);
            if (mem->size == 4) {
                Store<float>(a, static_cast<float>(ST(0)));
            } else if (mem->size == 8) {
                Store<double>(a, ST(0));
            } else {
                ToExtended(ST(0), Ptr(a, 10));
            }
        } else {
            ST(st_idx[0]) = ST(0);
        }
        if (in.mnemonic == ZYDIS_MNEMONIC_FSTP) {
            pop();
        }
        return true;
    }
    case ZYDIS_MNEMONIC_FIST:
    case ZYDIS_MNEMONIC_FISTP:
        store_int(*mem, RoundByControl(ST(0), s.fpu_cw));
        if (in.mnemonic == ZYDIS_MNEMONIC_FISTP) {
            pop();
        }
        return true;
    case ZYDIS_MNEMONIC_FISTTP:
        store_int(*mem, std::trunc(ST(0)));
        pop();
        return true;
    case ZYDIS_MNEMONIC_FLDZ:
        push(0.0);
        return true;
    case ZYDIS_MNEMONIC_FLD1:
        push(1.0);
        return true;
    case ZYDIS_MNEMONIC_FLDPI:
        push(3.14159265358979323846);
        return true;
    case ZYDIS_MNEMONIC_FLDL2E:
        push(1.44269504088896340736);
        return true;
    case ZYDIS_MNEMONIC_FLDL2T:
        push(3.32192809488736234787);
        return true;
    case ZYDIS_MNEMONIC_FLDLG2:
        push(0.30102999566398119521);
        return true;
    case ZYDIS_MNEMONIC_FLDLN2:
        push(0.69314718055994530942);
        return true;
    case ZYDIS_MNEMONIC_FXCH: {
        const int i = nst > 1 ? st_idx[1] : nst == 1 ? st_idx[0] : 1;
        std::swap(ST(0), ST(i == 0 && nst > 1 ? st_idx[0] : i));
        return true;
    }
    case ZYDIS_MNEMONIC_FCHS:
        ST(0) = -ST(0);
        return true;
    case ZYDIS_MNEMONIC_FABS:
        ST(0) = std::fabs(ST(0));
        return true;
    case ZYDIS_MNEMONIC_FSQRT:
        ST(0) = std::sqrt(ST(0));
        return true;
    case ZYDIS_MNEMONIC_FRNDINT:
        ST(0) = RoundByControl(ST(0), s.fpu_cw);
        return true;
    case ZYDIS_MNEMONIC_FSIN:
        ST(0) = std::sin(ST(0));
        set_cc(false, false, false);
        return true;
    case ZYDIS_MNEMONIC_FCOS:
        ST(0) = std::cos(ST(0));
        set_cc(false, false, false);
        return true;
    case ZYDIS_MNEMONIC_FSINCOS: {
        const double v = ST(0);
        ST(0) = std::sin(v);
        push(std::cos(v));
        set_cc(false, false, false);
        return true;
    }
    case ZYDIS_MNEMONIC_FPTAN:
        ST(0) = std::tan(ST(0));
        push(1.0);
        set_cc(false, false, false);
        return true;
    case ZYDIS_MNEMONIC_FPATAN:
        ST(1) = std::atan2(ST(1), ST(0));
        pop();
        return true;
    case ZYDIS_MNEMONIC_F2XM1:
        ST(0) = std::exp2(ST(0)) - 1.0;
        return true;
    case ZYDIS_MNEMONIC_FYL2X:
        ST(1) = ST(1) * std::log2(ST(0));
        pop();
        return true;
    case ZYDIS_MNEMONIC_FYL2XP1:
        ST(1) = ST(1) * std::log2(ST(0) + 1.0);
        pop();
        return true;
    case ZYDIS_MNEMONIC_FSCALE:
        ST(0) = std::ldexp(ST(0), static_cast<int>(std::trunc(ST(1))));
        return true;
    case ZYDIS_MNEMONIC_FPREM:
    case ZYDIS_MNEMONIC_FPREM1: {
        const double a = ST(0), b = ST(1);
        const double q = in.mnemonic == ZYDIS_MNEMONIC_FPREM ? std::trunc(a / b) : std::nearbyint(a / b);
        ST(0) = a - q * b;
        const u64 qi = static_cast<u64>(std::fabs(q));
        s.fpu_sw = (s.fpu_sw & ~0x4700) | ((qi & 1) ? 0x200 : 0) | ((qi & 2) ? 0x4000 : 0) |
                   ((qi & 4) ? 0x100 : 0);
        return true;
    }
    case ZYDIS_MNEMONIC_FXTRACT: {
        int e;
        const double m = std::frexp(ST(0), &e);
        ST(0) = static_cast<double>(e - 1);
        push(m * 2.0);
        return true;
    }
    case ZYDIS_MNEMONIC_FADD:
    case ZYDIS_MNEMONIC_FADDP:
    case ZYDIS_MNEMONIC_FIADD:
        arith([](double a, double b) { return a + b; }, false, in.mnemonic == ZYDIS_MNEMONIC_FADDP,
              in.mnemonic == ZYDIS_MNEMONIC_FIADD);
        return true;
    case ZYDIS_MNEMONIC_FMUL:
    case ZYDIS_MNEMONIC_FMULP:
    case ZYDIS_MNEMONIC_FIMUL:
        arith([](double a, double b) { return a * b; }, false, in.mnemonic == ZYDIS_MNEMONIC_FMULP,
              in.mnemonic == ZYDIS_MNEMONIC_FIMUL);
        return true;
    case ZYDIS_MNEMONIC_FSUB:
    case ZYDIS_MNEMONIC_FSUBP:
    case ZYDIS_MNEMONIC_FISUB:
        arith([](double a, double b) { return a - b; }, false, in.mnemonic == ZYDIS_MNEMONIC_FSUBP,
              in.mnemonic == ZYDIS_MNEMONIC_FISUB);
        return true;
    case ZYDIS_MNEMONIC_FSUBR:
    case ZYDIS_MNEMONIC_FSUBRP:
    case ZYDIS_MNEMONIC_FISUBR:
        arith([](double a, double b) { return a - b; }, true, in.mnemonic == ZYDIS_MNEMONIC_FSUBRP,
              in.mnemonic == ZYDIS_MNEMONIC_FISUBR);
        return true;
    case ZYDIS_MNEMONIC_FDIV:
    case ZYDIS_MNEMONIC_FDIVP:
    case ZYDIS_MNEMONIC_FIDIV:
        arith([](double a, double b) { return a / b; }, false, in.mnemonic == ZYDIS_MNEMONIC_FDIVP,
              in.mnemonic == ZYDIS_MNEMONIC_FIDIV);
        return true;
    case ZYDIS_MNEMONIC_FDIVR:
    case ZYDIS_MNEMONIC_FDIVRP:
    case ZYDIS_MNEMONIC_FIDIVR:
        arith([](double a, double b) { return a / b; }, true, in.mnemonic == ZYDIS_MNEMONIC_FDIVRP,
              in.mnemonic == ZYDIS_MNEMONIC_FIDIVR);
        return true;
    case ZYDIS_MNEMONIC_FCOM:
    case ZYDIS_MNEMONIC_FCOMP:
    case ZYDIS_MNEMONIC_FUCOM:
    case ZYDIS_MNEMONIC_FUCOMP:
    case ZYDIS_MNEMONIC_FICOM:
    case ZYDIS_MNEMONIC_FICOMP: {
        const bool int_mem =
            in.mnemonic == ZYDIS_MNEMONIC_FICOM || in.mnemonic == ZYDIS_MNEMONIC_FICOMP;
        const double b = mem ? (int_mem ? load_int(*mem) : load_fp(*mem))
                             : ST(nst > 1 ? st_idx[1] : nst == 1 && st_idx[0] != 0 ? st_idx[0] : 1);
        compare(ST(0), b, false);
        if (in.mnemonic == ZYDIS_MNEMONIC_FCOMP || in.mnemonic == ZYDIS_MNEMONIC_FUCOMP ||
            in.mnemonic == ZYDIS_MNEMONIC_FICOMP) {
            pop();
        }
        return true;
    }
    case ZYDIS_MNEMONIC_FCOMPP:
    case ZYDIS_MNEMONIC_FUCOMPP:
        compare(ST(0), ST(1), false);
        pop();
        pop();
        return true;
    case ZYDIS_MNEMONIC_FCOMI:
    case ZYDIS_MNEMONIC_FCOMIP:
    case ZYDIS_MNEMONIC_FUCOMI:
    case ZYDIS_MNEMONIC_FUCOMIP:
        compare(ST(0), ST(nst > 1 ? st_idx[1] : 1), true);
        if (in.mnemonic == ZYDIS_MNEMONIC_FCOMIP || in.mnemonic == ZYDIS_MNEMONIC_FUCOMIP) {
            pop();
        }
        return true;
    case ZYDIS_MNEMONIC_FTST:
        compare(ST(0), 0.0, false);
        return true;
    case ZYDIS_MNEMONIC_FXAM: {
        const double v = ST(0);
        const bool empty = s.fpu_tag_empty & (1u << phys(0));
        bool c0 = false, c2 = false, c3 = false;
        if (empty) {
            c0 = c3 = true;
        } else if (std::isnan(v)) {
            c0 = true;
        } else if (std::isinf(v)) {
            c0 = c2 = true;
        } else if (v == 0.0) {
            c3 = true;
        } else {
            c2 = true;
        }
        set_cc(c0, c2, c3);
        s.fpu_sw = (s.fpu_sw & ~0x200) | (std::signbit(v) ? 0x200 : 0);
        return true;
    }
    case ZYDIS_MNEMONIC_FCMOVB:
    case ZYDIS_MNEMONIC_FCMOVE:
    case ZYDIS_MNEMONIC_FCMOVBE:
    case ZYDIS_MNEMONIC_FCMOVU:
    case ZYDIS_MNEMONIC_FCMOVNB:
    case ZYDIS_MNEMONIC_FCMOVNE:
    case ZYDIS_MNEMONIC_FCMOVNBE:
    case ZYDIS_MNEMONIC_FCMOVNU: {
        const u64 f = s.rflags;
        bool c;
        switch (in.mnemonic) {
        case ZYDIS_MNEMONIC_FCMOVB:
            c = f & CF;
            break;
        case ZYDIS_MNEMONIC_FCMOVE:
            c = f & ZF;
            break;
        case ZYDIS_MNEMONIC_FCMOVBE:
            c = f & (CF | ZF);
            break;
        case ZYDIS_MNEMONIC_FCMOVU:
            c = f & PF;
            break;
        case ZYDIS_MNEMONIC_FCMOVNB:
            c = !(f & CF);
            break;
        case ZYDIS_MNEMONIC_FCMOVNE:
            c = !(f & ZF);
            break;
        case ZYDIS_MNEMONIC_FCMOVNBE:
            c = !(f & (CF | ZF));
            break;
        default:
            c = !(f & PF);
            break;
        }
        if (c) {
            ST(0) = ST(nst > 1 ? st_idx[1] : st_idx[0]);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_FLDCW:
        s.fpu_cw = Load<u16>(EffectiveAddress(*mem));
        return true;
    case ZYDIS_MNEMONIC_FNSTCW:
        Store<u16>(EffectiveAddress(*mem), s.fpu_cw);
        return true;
    case ZYDIS_MNEMONIC_FNSTSW: {
        const u16 sw = (s.fpu_sw & ~0x3800) | ((s.fpu_top & 7) << 11);
        if (mem) {
            Store<u16>(EffectiveAddress(*mem), sw);
        } else {
            WriteGpr(RAX, 2, sw);
        }
        return true;
    }
    case ZYDIS_MNEMONIC_FNINIT:
        s.fpu_cw = 0x037F;
        s.fpu_sw = 0;
        s.fpu_top = 0;
        s.fpu_tag_empty = 0xFF;
        return true;
    case ZYDIS_MNEMONIC_FNCLEX:
        s.fpu_sw &= 0x7F00;
        return true;
    case ZYDIS_MNEMONIC_FWAIT:
    case ZYDIS_MNEMONIC_FNOP:
        return true;
    case ZYDIS_MNEMONIC_FFREE:
        s.fpu_tag_empty |= 1u << phys(st_idx[0]);
        return true;
    case ZYDIS_MNEMONIC_FINCSTP:
        s.fpu_top = (s.fpu_top + 1) & 7;
        return true;
    case ZYDIS_MNEMONIC_FDECSTP:
        s.fpu_top = (s.fpu_top - 1) & 7;
        return true;
    case ZYDIS_MNEMONIC_FNSTENV:
    case ZYDIS_MNEMONIC_FLDENV: {
        u8* p = Ptr(EffectiveAddress(*mem), 28);
        if (in.mnemonic == ZYDIS_MNEMONIC_FNSTENV) {
            std::memset(p, 0, 28);
            const u16 sw = (s.fpu_sw & ~0x3800) | ((s.fpu_top & 7) << 11);
            std::memcpy(p, &s.fpu_cw, 2);
            std::memcpy(p + 4, &sw, 2);
        } else {
            std::memcpy(&s.fpu_cw, p, 2);
            u16 sw;
            std::memcpy(&sw, p + 4, 2);
            s.fpu_sw = sw & ~0x3800;
            s.fpu_top = (sw >> 11) & 7;
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace Core::Interpreter
