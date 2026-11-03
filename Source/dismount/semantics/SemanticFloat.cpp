/*
 * SemanticFloat.cpp
 *
 * A software floating point library, see SemanticFloat.h. A number is
 * unpacked into a class, a sign, an exponent and a 128 bit significand (the
 * most significant bit is the integer bit), the operation is computed in a
 * 256 bit integer and rounded once by roundPack().
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticFloat.h"

typedef SemanticFloat::Bits Bits;
typedef SemanticFloat::Environment Environment;
typedef SemanticOperation Op;

SemanticFloat::Environment::Environment() :
    m_rounding(SemanticOperation::ROUND_NEAREST_EVEN),
    m_precision(0),
    m_flushToZero(false),
    m_denormalsAreZero(false),
    m_isUnderflowUnmasked(false),
    m_isTininessBeforeRounding(false),
    m_nanRule(FloatEnvironment::NAN_FIRST_OPERAND),
    m_isDefaultNanNegative(true)
{
}

/////////////////////////////////////////////////////////////////////////////
// 256 bit integers
/////////////////////////////////////////////////////////////////////////////

namespace {

// An unsigned 256 bit integer, w[0] is the lowest word
struct W {
    uint64 w[4];
};

W wMake(uint64 w0 = 0, uint64 w1 = 0, uint64 w2 = 0, uint64 w3 = 0)
{
    W r;
    r.w[0] = w0; r.w[1] = w1; r.w[2] = w2; r.w[3] = w3;
    return r;
}

bool wIsZero(const W& a)
{
    return (a.w[0] | a.w[1] | a.w[2] | a.w[3]) == 0;
}

// Return -1, 0, 1
int wCmp(const W& a, const W& b)
{
    for (int i = 3; i >= 0; i--)
    {
        if (a.w[i] != b.w[i])
            return (a.w[i] < b.w[i]) ? -1 : 1;
    }
    return 0;
}

W wAdd(const W& a, const W& b)
{
    W r;
    uint64 carry = 0;
    for (uint i = 0; i < 4; i++)
    {
        const uint64 s = a.w[i] + b.w[i];
        const uint64 c1 = (s < a.w[i]) ? 1 : 0;
        r.w[i] = s + carry;
        const uint64 c2 = (r.w[i] < s) ? 1 : 0;
        carry = c1 | c2;
    }
    return r;
}

// a - b, a >= b
W wSub(const W& a, const W& b)
{
    W r;
    uint64 borrow = 0;
    for (uint i = 0; i < 4; i++)
    {
        const uint64 d = a.w[i] - b.w[i];
        const uint64 b1 = (a.w[i] < b.w[i]) ? 1 : 0;
        r.w[i] = d - borrow;
        const uint64 b2 = (d < borrow) ? 1 : 0;
        borrow = b1 | b2;
    }
    return r;
}

W wShl(const W& a, uint n)
{
    if (n >= 256)
        return wMake();
    const uint ws = n / 64, bs = n % 64;
    W r = wMake();
    for (int i = 3; i >= (int)ws; i--)
    {
        uint64 v = a.w[i - ws] << bs;
        if ((bs != 0) && (i - (int)ws - 1 >= 0))
            v |= a.w[i - ws - 1] >> (64 - bs);
        r.w[i] = v;
    }
    return r;
}

W wShr(const W& a, uint n)
{
    if (n >= 256)
        return wMake();
    const uint ws = n / 64, bs = n % 64;
    W r = wMake();
    for (uint i = 0; i + ws < 4; i++)
    {
        uint64 v = a.w[i + ws] >> bs;
        if ((bs != 0) && (i + ws + 1 < 4))
            v |= a.w[i + ws + 1] << (64 - bs);
        r.w[i] = v;
    }
    return r;
}

bool wBit(const W& a, uint i)
{
    if (i >= 256)
        return false;
    return ((a.w[i / 64] >> (i % 64)) & 1) != 0;
}

// True if any of the bits [0, count) is set
bool wAnyBelow(const W& a, uint count)
{
    if (count > 256)
        count = 256;
    for (uint i = 0; i < 4; i++)
    {
        if (count >= (i + 1) * 64)
        {
            if (a.w[i] != 0)
                return true;
        } else if (count > i * 64)
        {
            const uint bits = count - i * 64;
            if ((a.w[i] & (((uint64)1 << bits) - 1)) != 0)
                return true;
        }
    }
    return false;
}

// Shift right, the bits shifted out are ORed into bit 0
W wShrJam(const W& a, uint n)
{
    if (n == 0)
        return a;
    const bool isLost = wAnyBelow(a, n);
    W r = wShr(a, n);
    if (isLost)
        r.w[0] |= 1;
    return r;
}

// The number of leading zero bits, 256 for 0
uint wClz(const W& a)
{
    for (int i = 3; i >= 0; i--)
    {
        if (a.w[i] != 0)
        {
            uint n = 0;
            uint64 v = a.w[i];
            while ((v & ((uint64)1 << 63)) == 0)
            {
                v <<= 1;
                n++;
            }
            return (uint)(3 - i) * 64 + n;
        }
    }
    return 256;
}

// Keep the low n bits
W wLow(const W& a, uint n)
{
    if (n >= 256)
        return a;
    W r = a;
    for (uint i = 0; i < 4; i++)
    {
        if (n >= (i + 1) * 64)
            continue;
        if (n <= i * 64)
            r.w[i] = 0;
        else
            r.w[i] &= ((uint64)1 << (n - i * 64)) - 1;
    }
    return r;
}

// The unsigned product of two 64 bit numbers
void mul64(uint64 a, uint64 b, uint64& high, uint64& low)
{
    const uint64 low32 = 0xFFFFFFFF;
    const uint64 a0 = a & low32, a1 = a >> 32;
    const uint64 b0 = b & low32, b1 = b >> 32;
    const uint64 p00 = a0 * b0;
    const uint64 p01 = a0 * b1;
    const uint64 p10 = a1 * b0;
    const uint64 p11 = a1 * b1;
    const uint64 middle = (p00 >> 32) + (p01 & low32) + (p10 & low32);
    low = (p00 & low32) | (middle << 32);
    high = p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32);
}

// The 256 bit product of two 128 bit numbers
W wMul128(uint64 aHi, uint64 aLo, uint64 bHi, uint64 bLo)
{
    uint64 h00, l00, h01, l01, h10, l10, h11, l11;
    mul64(aLo, bLo, h00, l00);
    mul64(aLo, bHi, h01, l01);
    mul64(aHi, bLo, h10, l10);
    mul64(aHi, bHi, h11, l11);
    W r = wMake(l00, h00, l11, h11);
    r = wAdd(r, wMake(0, l01, h01, 0));
    r = wAdd(r, wMake(0, l10, h10, 0));
    return r;
}

/////////////////////////////////////////////////////////////////////////////
// Formats and unpacked numbers
/////////////////////////////////////////////////////////////////////////////

struct FormatInfo {
    uint m_total;
    uint m_expBits;
    // The explicit fraction bits (not including the integer bit)
    uint m_fracBits;
    int32 m_bias;
    // The significand bits including the integer bit
    uint m_precision;
    bool m_isExtended;
};

bool getInfo(SemanticFloat::Format format, FormatInfo& info)
{
    info.m_isExtended = false;
    switch (format)
    {
    case SemanticType::FLOAT_HALF:
        info.m_total = 16; info.m_expBits = 5; info.m_fracBits = 10;
        info.m_bias = 15; break;
    case SemanticType::FLOAT_BFLOAT16:
        info.m_total = 16; info.m_expBits = 8; info.m_fracBits = 7;
        info.m_bias = 127; break;
    case SemanticType::FLOAT_SINGLE:
        info.m_total = 32; info.m_expBits = 8; info.m_fracBits = 23;
        info.m_bias = 127; break;
    case SemanticType::FLOAT_DOUBLE:
        info.m_total = 64; info.m_expBits = 11; info.m_fracBits = 52;
        info.m_bias = 1023; break;
    case SemanticType::FLOAT_QUAD:
        info.m_total = 128; info.m_expBits = 15; info.m_fracBits = 112;
        info.m_bias = 16383; break;
    case SemanticType::FLOAT_EXTENDED:
        info.m_total = 80; info.m_expBits = 15; info.m_fracBits = 63;
        info.m_bias = 16383; info.m_isExtended = true; break;
    default:
        return false;
    }
    info.m_precision = info.m_fracBits + 1;
    return true;
}

FormatInfo infoOf(SemanticFloat::Format format)
{
    FormatInfo info;
    CHECK(getInfo(format, info));
    return info;
}

enum NumClass {
    CLS_ZERO,
    CLS_FINITE,
    CLS_INF,
    CLS_QNAN,
    CLS_SNAN,
    // An invalid x87 encoding
    CLS_INVALID
};

// An unpacked number. A finite number is sig / 2^127 * 2^exp, with the
// integer bit (bit 127 of hi:lo) set. A NaN keeps its payload in hi:lo, the
// quiet bit is bit 127
struct Num {
    int m_class;
    bool m_sign;
    bool m_isDenormal;
    int32 m_exp;
    uint64 m_hi;
    uint64 m_lo;
};

Num makeNum(int cls, bool sign)
{
    Num n;
    n.m_class = cls; n.m_sign = sign; n.m_isDenormal = false;
    n.m_exp = 0; n.m_hi = 0; n.m_lo = 0;
    return n;
}

bool isNanClass(int cls)
{
    return (cls == CLS_QNAN) || (cls == CLS_SNAN);
}

Num unpack(const FormatInfo& fi, const Bits& b, const Environment& env)
{
    Num n = makeNum(CLS_ZERO, false);
    const int32 emin = 1 - fi.m_bias;
    if (fi.m_isExtended)
    {
        n.m_sign = ((b.m_high >> 15) & 1) != 0;
        const uint expField = (uint)(b.m_high & 0x7FFF);
        const uint64 sig = b.m_low;
        const bool intBit = (sig >> 63) != 0;
        const uint64 frac = sig & (((uint64)1 << 63) - 1);
        if (expField == 0x7FFF)
        {
            if (!intBit)
            {
                n.m_class = CLS_INVALID;
            } else if (frac == 0)
            {
                n.m_class = CLS_INF;
            } else
            {
                n.m_class = ((frac >> 62) & 1) ? CLS_QNAN : CLS_SNAN;
                n.m_hi = frac << 1;
            }
            return n;
        }
        if (expField == 0)
        {
            if (intBit)
            {
                // A pseudo-denormal: the 387 and later handle it as a
                // denormal operand, its value is the significand at the
                // smallest exponent
                n.m_class = CLS_FINITE;
                n.m_isDenormal = true;
                n.m_hi = sig;
                n.m_exp = emin;
                return n;
            }
            if (frac == 0)
                return n;
            n.m_isDenormal = true;
            if (env.m_denormalsAreZero)
            {
                n.m_isDenormal = false;
                return n;
            }
            W v = wShl(wMake(frac), 64);
            const uint lz = wClz(v) - 128;
            v = wShl(v, lz);
            n.m_class = CLS_FINITE;
            n.m_hi = v.w[1]; n.m_lo = v.w[0];
            n.m_exp = emin - (int32)lz;
            return n;
        }
        if (!intBit)
        {
            n.m_class = CLS_INVALID;
            return n;
        }
        n.m_class = CLS_FINITE;
        n.m_hi = sig;
        n.m_exp = (int32)expField - fi.m_bias;
        return n;
    }

    const W v = wMake(b.m_low, b.m_high);
    n.m_sign = wBit(v, fi.m_total - 1);
    const W fracW = wLow(v, fi.m_fracBits);
    const uint expField = (uint)wLow(wShr(v, fi.m_fracBits),
                                     fi.m_expBits).w[0];
    const uint maxField = (1u << fi.m_expBits) - 1;
    if (expField == maxField)
    {
        if (wIsZero(fracW))
        {
            n.m_class = CLS_INF;
        } else
        {
            n.m_class = wBit(fracW, fi.m_fracBits - 1) ? CLS_QNAN : CLS_SNAN;
            const W p = wShl(fracW, 128 - fi.m_fracBits);
            n.m_hi = p.w[1]; n.m_lo = p.w[0];
        }
        return n;
    }
    if (expField == 0)
    {
        if (wIsZero(fracW))
            return n;
        if (env.m_denormalsAreZero)
            return n;
        n.m_isDenormal = true;
        W s = wShl(fracW, 128 - fi.m_fracBits);
        const uint lz = wClz(s) - 128;
        s = wShl(s, lz);
        n.m_class = CLS_FINITE;
        n.m_hi = s.w[1]; n.m_lo = s.w[0];
        n.m_exp = emin - 1 - (int32)lz;
        return n;
    }
    n.m_class = CLS_FINITE;
    const W s = wShl(wAdd(fracW, wShl(wMake(1), fi.m_fracBits)),
                     127 - fi.m_fracBits);
    n.m_hi = s.w[1]; n.m_lo = s.w[0];
    n.m_exp = (int32)expField - fi.m_bias;
    return n;
}

/////////////////////////////////////////////////////////////////////////////
// Packing
/////////////////////////////////////////////////////////////////////////////

Bits packFields(const FormatInfo& fi, bool sign, uint field, const W& frac,
                bool hasIntegerBit)
{
    if (fi.m_isExtended)
    {
        uint64 low = frac.w[0];
        if (hasIntegerBit)
            low |= (uint64)1 << 63;
        return Bits(low, (sign ? 0x8000u : 0u) | field);
    }
    W v = frac;
    v = wAdd(v, wShl(wMake(field), fi.m_fracBits));
    if (sign)
        v = wAdd(v, wShl(wMake(1), fi.m_total - 1));
    return Bits(v.w[0], v.w[1]);
}

Bits packZero(const FormatInfo& fi, bool sign)
{
    return packFields(fi, sign, 0, wMake(), false);
}

Bits packInf(const FormatInfo& fi, bool sign)
{
    return packFields(fi, sign, (1u << fi.m_expBits) - 1, wMake(), true);
}

// Pack a NaN, made quiet
Bits packNan(const FormatInfo& fi, bool sign, uint64 hi, uint64 lo)
{
    hi |= (uint64)1 << 63;
    const W frac = wShr(wMake(lo, hi), 128 - fi.m_fracBits);
    return packFields(fi, sign, (1u << fi.m_expBits) - 1, frac, true);
}

Num defaultNan(const Environment& env)
{
    Num n = makeNum(CLS_QNAN, env.m_isDefaultNanNegative);
    n.m_hi = (uint64)1 << 63;
    return n;
}

// The precision of an arithmetic operation result
uint effectivePrecision(const FormatInfo& fi, const Environment& env)
{
    if (fi.m_isExtended && ((env.m_precision == 24) ||
                            (env.m_precision == 53) ||
                            (env.m_precision == 64)))
        return env.m_precision;
    return fi.m_precision;
}

Op::RoundingMode modeOf(const Environment& env)
{
    return (env.m_rounding == Op::ROUND_DYNAMIC) ? Op::ROUND_NEAREST_EVEN
                                                 : env.m_rounding;
}

struct Rounded {
    W m_value;
    bool m_isInexact;
    // The value became 2^keep
    bool m_isCarry;
};

/*
 * Keep the top 'keep' bits of sig (the most significant bit is bit 255) and
 * round the rest. 'keep' may be 0 or negative (the value is below the unit)
 */
Rounded roundSignificand(const W& sig, int32 keep, bool sign,
                         Op::RoundingMode mode)
{
    Rounded r;
    r.m_value = wMake();
    r.m_isCarry = false;
    bool roundBit, sticky;
    if (keep >= 1)
    {
        r.m_value = wShr(sig, 256 - keep);
        roundBit = wBit(sig, 255 - keep);
        sticky = wAnyBelow(sig, 255 - keep);
    } else if (keep == 0)
    {
        roundBit = wBit(sig, 255);
        sticky = wAnyBelow(sig, 255);
    } else
    {
        roundBit = false;
        sticky = true;
    }
    r.m_isInexact = roundBit || sticky;
    const bool lsb = (r.m_value.w[0] & 1) != 0;
    bool increment = false;
    switch (mode)
    {
    case Op::ROUND_NEAREST_AWAY:
        increment = roundBit; break;
    case Op::ROUND_TOWARD_ZERO:
        break;
    case Op::ROUND_TOWARD_POSITIVE:
        increment = !sign && r.m_isInexact; break;
    case Op::ROUND_TOWARD_NEGATIVE:
        increment = sign && r.m_isInexact; break;
    case Op::ROUND_TO_ODD:
        if (r.m_isInexact)
            r.m_value.w[0] |= 1;
        break;
    default:
        increment = roundBit && (sticky || lsb); break;
    }
    if (increment)
    {
        r.m_value = wAdd(r.m_value, wMake(1));
        r.m_isCarry = (keep >= 1) && wBit(r.m_value, keep);
    }
    return r;
}

/*
 * Round a finite non zero number (sig has its most significant bit at bit
 * 255, the value is sig / 2^255 * 2^exp, the lower bits may hold a sticky
 * bit) to the format and pack it.
 */
Bits roundPack(const FormatInfo& fi, const Environment& env, bool sign,
               int32 exp, const W& sig, uint p, uint32& flags)
{
    const Op::RoundingMode mode = modeOf(env);
    const int32 emin = 1 - fi.m_bias;
    const int32 emax = fi.m_bias;
    int32 keep = (int32)p;
    const bool isDenormalRange = exp < emin;
    if (isDenormalRange)
    {
        const int64 d = (int64)emin - exp;
        keep = (d > (int64)p + 2) ? -2 : (int32)((int64)p - d);
    }
    Rounded r = roundSignificand(sig, keep, sign, mode);

    bool isTiny = false;
    if (isDenormalRange)
    {
        isTiny = true;
        if (!env.m_isTininessBeforeRounding && (exp == emin - 1))
        {
            // Tiny after rounding: rounding with an unbounded exponent range
            // doesn't reach the smallest normal number
            if (roundSignificand(sig, (int32)p, sign, mode).m_isCarry)
                isTiny = false;
        }
    }
    if (isTiny && env.m_flushToZero && !env.m_isUnderflowUnmasked)
    {
        flags |= SemanticFloatFlags::FLOAT_UNDERFLOW |
                 SemanticFloatFlags::FLOAT_INEXACT;
        return packZero(fi, sign);
    }
    if (r.m_isInexact)
        flags |= SemanticFloatFlags::FLOAT_INEXACT;
    if (isTiny && (r.m_isInexact || env.m_isUnderflowUnmasked))
        flags |= SemanticFloatFlags::FLOAT_UNDERFLOW;

    W m = r.m_value;
    uint field;
    if (!isDenormalRange)
    {
        if (r.m_isCarry)
        {
            m = wShr(m, 1);
            exp++;
        }
        if (exp > emax)
        {
            flags |= SemanticFloatFlags::FLOAT_OVERFLOW |
                     SemanticFloatFlags::FLOAT_INEXACT;
            bool isInfinity;
            switch (mode)
            {
            case Op::ROUND_TOWARD_ZERO:
            case Op::ROUND_TO_ODD:
                isInfinity = false; break;
            case Op::ROUND_TOWARD_POSITIVE:
                isInfinity = !sign; break;
            case Op::ROUND_TOWARD_NEGATIVE:
                isInfinity = sign; break;
            default:
                isInfinity = true; break;
            }
            if (isInfinity)
                return packInf(fi, sign);
            // The largest finite number of the precision
            const W maxM = wSub(wShl(wMake(1), p), wMake(1));
            const W frac = fi.m_isExtended ? wShl(maxM, 64 - p) : wLow(maxM, p - 1);
            return packFields(fi, sign, (1u << fi.m_expBits) - 2, frac, true);
        }
        field = (uint)(exp + fi.m_bias);
    } else
    {
        if (wIsZero(m))
            return packZero(fi, sign);
        field = wBit(m, p - 1) ? 1 : 0;
    }
    if (fi.m_isExtended)
        return packFields(fi, sign, field, wShl(m, 64 - p), false);
    return packFields(fi, sign, field, wLow(m, p - 1), false);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// NaNs and the building blocks of the operations
/////////////////////////////////////////////////////////////////////////////

namespace {

typedef SemanticFloatFlags FF;

bool hasNanOrInvalid(const Num* ops, uint count)
{
    for (uint i = 0; i < count; i++)
    {
        if (isNanClass(ops[i].m_class) || (ops[i].m_class == CLS_INVALID))
            return true;
    }
    return false;
}

// x87: the NaN with the larger significand, a quiet NaN before a signaling one
const Num& pickX87(const Num& a, const Num& b)
{
    if (a.m_class == CLS_SNAN)
    {
        if (b.m_class != CLS_SNAN)
            return b;
    } else if (b.m_class == CLS_SNAN)
    {
        return a;
    }
    if (a.m_hi != b.m_hi)
        return (a.m_hi < b.m_hi) ? b : a;
    if (a.m_lo != b.m_lo)
        return (a.m_lo < b.m_lo) ? b : a;
    // The positive one
    return a.m_sign ? b : a;
}

/*
 * The result of an operation which has a NaN (or an invalid encoding)
 * operand. A signaling NaN raises FLOAT_INVALID
 */
Num propagateNan(const Environment& env, const Num* ops, uint count,
                 uint32& flags)
{
    for (uint i = 0; i < count; i++)
    {
        if (ops[i].m_class == CLS_INVALID)
        {
            flags |= FF::FLOAT_INVALID;
            return defaultNan(env);
        }
    }
    bool isSignaling = false;
    for (uint i = 0; i < count; i++)
        isSignaling = isSignaling || (ops[i].m_class == CLS_SNAN);
    if (isSignaling)
        flags |= FF::FLOAT_INVALID;
    if (env.m_nanRule == FloatEnvironment::NAN_DEFAULT)
        return defaultNan(env);

    const Num* chosen = NULL;
    if (env.m_nanRule == FloatEnvironment::NAN_SIGNALING_FIRST)
    {
        for (uint i = 0; (i < count) && (chosen == NULL); i++)
        {
            if (ops[i].m_class == CLS_SNAN)
                chosen = &ops[i];
        }
    }
    for (uint i = 0; i < count; i++)
    {
        if (!isNanClass(ops[i].m_class))
            continue;
        if (chosen == NULL)
        {
            chosen = &ops[i];
            if (env.m_nanRule != FloatEnvironment::NAN_LARGER_SIGNIFICAND)
                break;
        } else if (env.m_nanRule == FloatEnvironment::NAN_LARGER_SIGNIFICAND)
        {
            chosen = &pickX87(*chosen, ops[i]);
        }
    }
    Num r = *chosen;
    r.m_class = CLS_QNAN;
    r.m_hi |= (uint64)1 << 63;
    return r;
}

// Raise the denormal operand flag
void noteDenormals(const Num* ops, uint count, uint32& flags)
{
    for (uint i = 0; i < count; i++)
    {
        if (ops[i].m_isDenormal)
            flags |= FF::FLOAT_DENORMAL;
    }
}

// Pack a number which isn't finite (or is a zero)
Bits packSpecial(const FormatInfo& fi, const Num& n)
{
    switch (n.m_class)
    {
    case CLS_ZERO: return packZero(fi, n.m_sign);
    case CLS_INF:  return packInf(fi, n.m_sign);
    default:       return packNan(fi, n.m_sign, n.m_hi, n.m_lo);
    }
}

// Round and pack a finite non zero number given as a 128 bit significand
Bits packFinite(const FormatInfo& fi, const Environment& env, bool sign,
                int32 exp, uint64 hi, uint64 lo, bool isSticky, uint p,
                uint32& flags)
{
    W sig = wShl(wMake(lo, hi), 128);
    if (isSticky)
        sig.w[0] |= 1;
    return roundPack(fi, env, sign, exp, sig, p, flags);
}

// A number with a 256 bit significand, the most significant bit is bit 254
struct Wide {
    bool m_isZero;
    bool m_sign;
    int32 m_exp;
    W m_sig;
};

Wide wideOf(const Num& n)
{
    Wide w;
    w.m_isZero = (n.m_class == CLS_ZERO);
    w.m_sign = n.m_sign;
    w.m_exp = n.m_exp;
    w.m_sig = wShl(wMake(n.m_lo, n.m_hi), 127);
    return w;
}

// Add two numbers exactly (a sticky bit for what is far below), and round
Bits addWide(const FormatInfo& fi, const Environment& env, Wide a, Wide b,
             uint p, uint32& flags)
{
    if (a.m_isZero && b.m_isZero)
    {
        const bool sign = (a.m_sign == b.m_sign) ? a.m_sign
                            : (modeOf(env) == Op::ROUND_TOWARD_NEGATIVE);
        return packZero(fi, sign);
    }
    if (a.m_isZero)
        a = b;
    else if (!b.m_isZero)
    {
        if ((a.m_exp < b.m_exp) ||
            ((a.m_exp == b.m_exp) && (wCmp(a.m_sig, b.m_sig) < 0)))
        {
            const Wide t = a; a = b; b = t;
        }
        const int64 d = (int64)a.m_exp - b.m_exp;
        const W bs = (d >= 256) ? (wIsZero(b.m_sig) ? wMake() : wMake(1))
                                : wShrJam(b.m_sig, (uint)d);
        if (a.m_sign == b.m_sign)
        {
            a.m_sig = wAdd(a.m_sig, bs);
        } else
        {
            if (wCmp(a.m_sig, bs) == 0)
                return packZero(fi, modeOf(env) == Op::ROUND_TOWARD_NEGATIVE);
            a.m_sig = wSub(a.m_sig, bs);
        }
    }
    const uint lz = wClz(a.m_sig);
    return roundPack(fi, env, a.m_sign, a.m_exp + 1 - (int32)lz,
                     wShl(a.m_sig, lz), p, flags);
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// Formats and encodings
/////////////////////////////////////////////////////////////////////////////

bool SemanticFloat::isSupported(Format format)
{
    FormatInfo info;
    return getInfo(format, info);
}

uint SemanticFloat::getTotalBits(Format format)
{
    return infoOf(format).m_total;
}

uint SemanticFloat::getPrecision(Format format)
{
    return infoOf(format).m_precision;
}

bool SemanticFloat::isValidEncoding(Format format, const Bits& value)
{
    return unpack(infoOf(format), value, Environment()).m_class != CLS_INVALID;
}

bool SemanticFloat::isNan(Format format, const Bits& value)
{
    return isNanClass(unpack(infoOf(format), value, Environment()).m_class);
}

bool SemanticFloat::isSignalingNan(Format format, const Bits& value)
{
    return unpack(infoOf(format), value, Environment()).m_class == CLS_SNAN;
}

bool SemanticFloat::isInfinity(Format format, const Bits& value)
{
    return unpack(infoOf(format), value, Environment()).m_class == CLS_INF;
}

bool SemanticFloat::isZero(Format format, const Bits& value)
{
    return unpack(infoOf(format), value, Environment()).m_class == CLS_ZERO;
}

bool SemanticFloat::isDenormal(Format format, const Bits& value)
{
    return unpack(infoOf(format), value, Environment()).m_isDenormal;
}

bool SemanticFloat::isNegative(Format format, const Bits& value)
{
    const FormatInfo fi = infoOf(format);
    if (fi.m_isExtended)
        return ((value.m_high >> 15) & 1) != 0;
    return wBit(wMake(value.m_low, value.m_high), fi.m_total - 1);
}

Bits SemanticFloat::makeZero(Format format, bool isNegative)
{
    return packZero(infoOf(format), isNegative);
}

Bits SemanticFloat::makeInfinity(Format format, bool isNegative)
{
    return packInf(infoOf(format), isNegative);
}

Bits SemanticFloat::makeDefaultNan(Format format, const Environment& env)
{
    return packSpecial(infoOf(format), defaultNan(env));
}

uint SemanticFloat::classify(Format format, const Bits& value)
{
    const Num n = unpack(infoOf(format), value, Environment());
    switch (n.m_class)
    {
    case CLS_ZERO:
        return n.m_sign ? CLASS_NEGATIVE_ZERO : CLASS_POSITIVE_ZERO;
    case CLS_INF:
        return n.m_sign ? CLASS_NEGATIVE_INFINITY : CLASS_POSITIVE_INFINITY;
    case CLS_QNAN:
        return CLASS_QUIET_NAN;
    case CLS_SNAN:
        return CLASS_SIGNALING_NAN;
    case CLS_FINITE:
        if (n.m_isDenormal)
            return n.m_sign ? CLASS_NEGATIVE_DENORMAL : CLASS_POSITIVE_DENORMAL;
        return n.m_sign ? CLASS_NEGATIVE_NORMAL : CLASS_POSITIVE_NORMAL;
    default:
        return 0;
    }
}

/////////////////////////////////////////////////////////////////////////////
// Sign operations
/////////////////////////////////////////////////////////////////////////////

Bits SemanticFloat::negate(Format format, const Bits& a)
{
    const FormatInfo fi = infoOf(format);
    Bits r = a;
    if (fi.m_isExtended)
        r.m_high ^= 0x8000;
    else if (fi.m_total == 128)
        r.m_high ^= (uint64)1 << 63;
    else
        r.m_low ^= (uint64)1 << (fi.m_total - 1);
    return r;
}

Bits SemanticFloat::absolute(Format format, const Bits& a)
{
    return isNegative(format, a) ? negate(format, a) : a;
}

Bits SemanticFloat::copySign(Format format, const Bits& a, const Bits& b)
{
    return (isNegative(format, a) != isNegative(format, b)) ? negate(format, a)
                                                            : a;
}

/////////////////////////////////////////////////////////////////////////////
// Arithmetic
/////////////////////////////////////////////////////////////////////////////

namespace {

Bits addSubtract(SemanticFloat::Format format, const Environment& env,
                 const Bits& a, const Bits& b, bool isSubtract,
                 uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    Num ops[2] = { unpack(fi, a, env), unpack(fi, b, env) };
    if (hasNanOrInvalid(ops, 2))
        return packSpecial(fi, propagateNan(env, ops, 2, flags));
    noteDenormals(ops, 2, flags);
    if (isSubtract)
        ops[1].m_sign = !ops[1].m_sign;
    const Num& x = ops[0];
    const Num& y = ops[1];
    if ((x.m_class == CLS_INF) || (y.m_class == CLS_INF))
    {
        if ((x.m_class == CLS_INF) && (y.m_class == CLS_INF) &&
            (x.m_sign != y.m_sign))
        {
            flags |= FF::FLOAT_INVALID;
            return packSpecial(fi, defaultNan(env));
        }
        return packInf(fi, (x.m_class == CLS_INF) ? x.m_sign : y.m_sign);
    }
    return addWide(fi, env, wideOf(x), wideOf(y),
                   effectivePrecision(fi, env), flags);
}

} // namespace

Bits SemanticFloat::add(Format format, const Environment& env,
                        const Bits& a, const Bits& b, uint32& flags)
{
    return addSubtract(format, env, a, b, false, flags);
}

Bits SemanticFloat::subtract(Format format, const Environment& env,
                             const Bits& a, const Bits& b, uint32& flags)
{
    return addSubtract(format, env, a, b, true, flags);
}

Bits SemanticFloat::multiply(Format format, const Environment& env,
                             const Bits& a, const Bits& b, uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num ops[2] = { unpack(fi, a, env), unpack(fi, b, env) };
    if (hasNanOrInvalid(ops, 2))
        return packSpecial(fi, propagateNan(env, ops, 2, flags));
    noteDenormals(ops, 2, flags);
    const Num& x = ops[0];
    const Num& y = ops[1];
    const bool sign = x.m_sign != y.m_sign;
    if ((x.m_class == CLS_INF) || (y.m_class == CLS_INF))
    {
        if ((x.m_class == CLS_ZERO) || (y.m_class == CLS_ZERO))
        {
            flags |= FF::FLOAT_INVALID;
            return packSpecial(fi, defaultNan(env));
        }
        return packInf(fi, sign);
    }
    if ((x.m_class == CLS_ZERO) || (y.m_class == CLS_ZERO))
        return packZero(fi, sign);
    W p = wMul128(x.m_hi, x.m_lo, y.m_hi, y.m_lo);
    int32 exp = x.m_exp + y.m_exp;
    if (wBit(p, 255))
        exp++;
    else
        p = wShl(p, 1);
    return roundPack(fi, env, sign, exp, p, effectivePrecision(fi, env),
                     flags);
}

Bits SemanticFloat::divide(Format format, const Environment& env,
                           const Bits& a, const Bits& b, uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num ops[2] = { unpack(fi, a, env), unpack(fi, b, env) };
    if (hasNanOrInvalid(ops, 2))
        return packSpecial(fi, propagateNan(env, ops, 2, flags));
    const Num& x = ops[0];
    const Num& y = ops[1];
    // A division by zero (or 0 / 0) is detected before the denormal operand
    // of the dividend: only the divide by zero or the invalid flag is raised
    if (y.m_class != CLS_ZERO)
        noteDenormals(ops, 2, flags);
    const bool sign = x.m_sign != y.m_sign;
    if (x.m_class == CLS_INF)
    {
        if (y.m_class == CLS_INF)
        {
            flags |= FF::FLOAT_INVALID;
            return packSpecial(fi, defaultNan(env));
        }
        return packInf(fi, sign);
    }
    if (y.m_class == CLS_INF)
        return packZero(fi, sign);
    if (y.m_class == CLS_ZERO)
    {
        if (x.m_class == CLS_ZERO)
        {
            flags |= FF::FLOAT_INVALID;
            return packSpecial(fi, defaultNan(env));
        }
        flags |= FF::FLOAT_DIVIDE_BY_ZERO;
        return packInf(fi, sign);
    }
    if (x.m_class == CLS_ZERO)
        return packZero(fi, sign);

    // q = floor(sx * 2^127 / sy), a restoring division
    const W divisor = wMake(y.m_lo, y.m_hi);
    W remainder = wMake();
    W q = wMake();
    for (int i = 254; i >= 0; i--)
    {
        remainder = wShl(remainder, 1);
        if ((i >= 127) && wBit(wMake(x.m_lo, x.m_hi), (uint)(i - 127)))
            remainder.w[0] |= 1;
        q = wShl(q, 1);
        if (wCmp(remainder, divisor) >= 0)
        {
            remainder = wSub(remainder, divisor);
            q.w[0] |= 1;
        }
    }
    int32 exp = x.m_exp - y.m_exp;
    if (!wBit(q, 127))
    {
        q = wShl(q, 1);
        exp--;
    }
    W sig = wShl(q, 128);
    if (!wIsZero(remainder))
        sig.w[0] |= 1;
    return roundPack(fi, env, sign, exp, sig, effectivePrecision(fi, env),
                     flags);
}

Bits SemanticFloat::squareRoot(Format format, const Environment& env,
                               const Bits& a, uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num x = unpack(fi, a, env);
    if (hasNanOrInvalid(&x, 1))
        return packSpecial(fi, propagateNan(env, &x, 1, flags));
    if (x.m_class == CLS_ZERO)
        return packZero(fi, x.m_sign);
    if (x.m_sign)
    {
        flags |= FF::FLOAT_INVALID;
        return packSpecial(fi, defaultNan(env));
    }
    noteDenormals(&x, 1, flags);
    if (x.m_class == CLS_INF)
        return packInf(fi, false);

    const int32 par = (x.m_exp & 1);
    const int32 half = (x.m_exp - par) / 2;
    W num = wShl(wMake(x.m_lo, x.m_hi), 127 + (uint)par);
    W res = wMake();
    W bit = wShl(wMake(1), 254);
    while (wCmp(bit, num) > 0)
        bit = wShr(bit, 2);
    while (!wIsZero(bit))
    {
        const W t = wAdd(res, bit);
        if (wCmp(num, t) >= 0)
        {
            num = wSub(num, t);
            res = wAdd(wShr(res, 1), bit);
        } else
        {
            res = wShr(res, 1);
        }
        bit = wShr(bit, 2);
    }
    W sig = wShl(res, 128);
    if (!wIsZero(num))
        sig.w[0] |= 1;
    return roundPack(fi, env, false, half, sig, effectivePrecision(fi, env),
                     flags);
}

Bits SemanticFloat::multiplyAdd(Format format, const Environment& env,
                                const Bits& a, const Bits& b, const Bits& c,
                                uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num ops[3] = { unpack(fi, a, env), unpack(fi, b, env),
                         unpack(fi, c, env) };
    const Num& x = ops[0];
    const Num& y = ops[1];
    const Num& z = ops[2];
    const bool isInvalidProduct =
        ((x.m_class == CLS_INF) && (y.m_class == CLS_ZERO)) ||
        ((x.m_class == CLS_ZERO) && (y.m_class == CLS_INF));
    if (isInvalidProduct)
    {
        flags |= FF::FLOAT_INVALID;
        return packSpecial(fi, defaultNan(env));
    }
    if (hasNanOrInvalid(ops, 3))
        return packSpecial(fi, propagateNan(env, ops, 3, flags));
    noteDenormals(ops, 3, flags);
    const bool productSign = x.m_sign != y.m_sign;
    if ((x.m_class == CLS_INF) || (y.m_class == CLS_INF))
    {
        if ((z.m_class == CLS_INF) && (z.m_sign != productSign))
        {
            flags |= FF::FLOAT_INVALID;
            return packSpecial(fi, defaultNan(env));
        }
        return packInf(fi, productSign);
    }
    if (z.m_class == CLS_INF)
        return packInf(fi, z.m_sign);

    const uint p = effectivePrecision(fi, env);
    Wide product;
    product.m_sign = productSign;
    if ((x.m_class == CLS_ZERO) || (y.m_class == CLS_ZERO))
    {
        product.m_isZero = true;
        product.m_exp = 0;
        product.m_sig = wMake();
    } else
    {
        product.m_isZero = false;
        W pr = wMul128(x.m_hi, x.m_lo, y.m_hi, y.m_lo);
        product.m_exp = x.m_exp + y.m_exp;
        if (wBit(pr, 255))
        {
            pr = wShr(pr, 1);
            product.m_exp++;
        }
        product.m_sig = pr;
    }
    return addWide(fi, env, product, wideOf(z), p, flags);
}

/////////////////////////////////////////////////////////////////////////////
// Rounding to integers, conversions
/////////////////////////////////////////////////////////////////////////////

namespace {

// Environment for the operations which ignore the precision control
Environment withoutPrecision(const Environment& env)
{
    Environment e(env);
    e.m_precision = 0;
    return e;
}

// Pack an integer value (not zero) given in 'value' as a finite number
Bits packInteger(const FormatInfo& fi, const Environment& env, bool sign,
                 const W& value, uint32& flags)
{
    const uint lz = wClz(value);
    return roundPack(fi, env, sign, (int32)(255 - lz), wShl(value, lz),
                     fi.m_precision, flags);
}

} // namespace

Bits SemanticFloat::roundToIntegral(Format format, const Environment& env,
                                    const Bits& a, bool isInexactSignaled,
                                    uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num x = unpack(fi, a, env);
    if (hasNanOrInvalid(&x, 1))
        return packSpecial(fi, propagateNan(env, &x, 1, flags));
    noteDenormals(&x, 1, flags);
    if ((x.m_class == CLS_ZERO) || (x.m_class == CLS_INF))
        return packSpecial(fi, x);
    const Environment e = withoutPrecision(env);
    const W sig = wShl(wMake(x.m_lo, x.m_hi), 128);
    if (x.m_exp >= (int32)fi.m_precision - 1)
        return roundPack(fi, e, x.m_sign, x.m_exp, sig, fi.m_precision, flags);
    const Rounded r = roundSignificand(sig, x.m_exp + 1, x.m_sign,
                                       modeOf(env));
    if (r.m_isInexact && isInexactSignaled)
        flags |= FF::FLOAT_INEXACT;
    if (wIsZero(r.m_value))
        return packZero(fi, x.m_sign);
    return packInteger(fi, e, x.m_sign, r.m_value, flags);
}

Bits SemanticFloat::convert(Format from, Format to, const Environment& env,
                            const Bits& a, uint32& flags)
{
    const FormatInfo fi = infoOf(from);
    const FormatInfo ti = infoOf(to);
    const Num x = unpack(fi, a, env);
    if (hasNanOrInvalid(&x, 1))
        return packSpecial(ti, propagateNan(env, &x, 1, flags));
    noteDenormals(&x, 1, flags);
    if ((x.m_class == CLS_ZERO) || (x.m_class == CLS_INF))
        return packSpecial(ti, x);
    return packFinite(ti, withoutPrecision(env), x.m_sign, x.m_exp, x.m_hi,
                      x.m_lo, false, ti.m_precision, flags);
}

Bits SemanticFloat::fromInteger(Format format, const Environment& env,
                                uint64 value, uint integerBits, bool isSigned,
                                uint32& flags)
{
    CHECK((integerBits >= 1) && (integerBits <= 64));
    const FormatInfo fi = infoOf(format);
    if (integerBits < 64)
        value &= ((uint64)1 << integerBits) - 1;
    bool sign = false;
    if (isSigned && (((value >> (integerBits - 1)) & 1) != 0))
    {
        sign = true;
        if (integerBits < 64)
            value |= ~(uint64)0 << integerBits;
        value = (uint64)0 - value;
    }
    if (value == 0)
        return packZero(fi, false);
    return packInteger(fi, withoutPrecision(env), sign, wMake(value), flags);
}

uint64 SemanticFloat::toInteger(Format format, const Environment& env,
                                const Bits& a, uint integerBits,
                                bool isSigned,
                                SemanticOperation::Behavior behavior,
                                uint32& flags)
{
    CHECK((integerBits >= 1) && (integerBits <= 64));
    const FormatInfo fi = infoOf(format);
    const Num x = unpack(fi, a, env);
    const uint64 mask = (integerBits == 64) ? ~(uint64)0
                                            : (((uint64)1 << integerBits) - 1);
    const uint64 maxValue = isSigned ? (mask >> 1) : mask;
    const uint64 minValue = isSigned ? (maxValue + 1) : 0;
    const uint64 indefinite = isSigned ? minValue : mask;
    const bool isSaturating =
        (behavior == Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO) ||
        (behavior == Op::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM);

    // The result of an invalid operation
    bool isNegativeOverflow = false;
    bool isInvalid = false;
    if (isNanClass(x.m_class) || (x.m_class == CLS_INVALID))
    {
        flags |= FF::FLOAT_INVALID;
        if (!isSaturating || (x.m_class == CLS_INVALID))
            return indefinite;
        return (behavior == Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO) ?
                    0 : maxValue;
    }
    // The conversions to an integer don't raise the denormal operand
    // exception: the SDM lists the invalid and the precision exceptions of
    // cvtss2si, cvttps2dq, cvtpd2pi... and of fist
    if (x.m_class == CLS_ZERO)
        return 0;
    uint64 magnitude = 0;
    if (x.m_class == CLS_INF)
    {
        isInvalid = true;
        isNegativeOverflow = x.m_sign;
    } else if (x.m_exp >= 64)
    {
        isInvalid = true;
        isNegativeOverflow = x.m_sign;
    } else
    {
        const W sig = wShl(wMake(x.m_lo, x.m_hi), 128);
        const Rounded r = roundSignificand(sig, x.m_exp + 1, x.m_sign,
                                           modeOf(env));
        magnitude = r.m_value.w[0];
        bool isFits;
        if (r.m_value.w[1] != 0)
            isFits = false;
        else if (isSigned)
            isFits = x.m_sign ? (magnitude <= minValue)
                              : (magnitude <= maxValue);
        else
            isFits = x.m_sign ? (magnitude == 0) : (magnitude <= maxValue);
        if (isFits)
        {
            if (r.m_isInexact)
                flags |= FF::FLOAT_INEXACT;
            return x.m_sign ? (((uint64)0 - magnitude) & mask) : magnitude;
        }
        isInvalid = true;
        isNegativeOverflow = x.m_sign;
    }
    CHECK(isInvalid);
    flags |= FF::FLOAT_INVALID;
    if (!isSaturating)
        return indefinite;
    return isNegativeOverflow ? minValue : maxValue;
}

/////////////////////////////////////////////////////////////////////////////
// Compare, minimum and maximum
/////////////////////////////////////////////////////////////////////////////

namespace {

// Compare two numbers which are not NaNs. Return -1, 0, 1
int compareNumbers(const Num& x, const Num& y)
{
    if ((x.m_class == CLS_ZERO) && (y.m_class == CLS_ZERO))
        return 0;
    if (x.m_sign != y.m_sign)
        return x.m_sign ? -1 : 1;
    int magnitude;
    if (x.m_class != y.m_class)
    {
        // zero < finite < infinity
        magnitude = (x.m_class < y.m_class) ? -1 : 1;
    } else if (x.m_class == CLS_INF)
    {
        magnitude = 0;
    } else if (x.m_exp != y.m_exp)
    {
        magnitude = (x.m_exp < y.m_exp) ? -1 : 1;
    } else if (x.m_hi != y.m_hi)
    {
        magnitude = (x.m_hi < y.m_hi) ? -1 : 1;
    } else if (x.m_lo != y.m_lo)
    {
        magnitude = (x.m_lo < y.m_lo) ? -1 : 1;
    } else
    {
        magnitude = 0;
    }
    return x.m_sign ? -magnitude : magnitude;
}

} // namespace

SemanticFloat::Relation SemanticFloat::compare(Format format,
                                               const Environment& env,
                                               const Bits& a, const Bits& b,
                                               bool isSignaling,
                                               uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num ops[2] = { unpack(fi, a, env), unpack(fi, b, env) };
    if (hasNanOrInvalid(ops, 2))
    {
        bool isInvalid = isSignaling;
        for (uint i = 0; i < 2; i++)
            isInvalid = isInvalid || (ops[i].m_class == CLS_SNAN) ||
                        (ops[i].m_class == CLS_INVALID);
        if (isInvalid)
            flags |= FF::FLOAT_INVALID;
        return RELATION_UNORDERED;
    }
    noteDenormals(ops, 2, flags);
    const int c = compareNumbers(ops[0], ops[1]);
    return (c < 0) ? RELATION_LESS : ((c == 0) ? RELATION_EQUAL
                                               : RELATION_GREATER);
}

Bits SemanticFloat::minimumMaximum(Format format, const Environment& env,
                                   bool isMaximum,
                                   SemanticOperation::Behavior behavior,
                                   const Bits& a, const Bits& b,
                                   uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num ops[2] = { unpack(fi, a, env), unpack(fi, b, env) };
    // The operands as the processor sees them: a denormal operand with DAZ
    // was converted to a zero of its sign before the computation, and the
    // result can be that zero
    const Bits operands[2] = {
        (ops[0].m_class == CLS_ZERO) ? packZero(fi, ops[0].m_sign) : a,
        (ops[1].m_class == CLS_ZERO) ? packZero(fi, ops[1].m_sign) : b };
    const bool hasNan = hasNanOrInvalid(ops, 2);
    if (hasNan)
    {
        if (behavior == Op::BEHAVIOR_MINMAX_SECOND)
        {
            // minps: the second operand, the ordered compare signals
            flags |= FF::FLOAT_INVALID;
            return operands[1];
        }
        if (behavior == Op::BEHAVIOR_MINMAX_NUMBER &&
            (ops[0].m_class != CLS_INVALID) && (ops[1].m_class != CLS_INVALID))
        {
            const bool isSignaling = (ops[0].m_class == CLS_SNAN) ||
                                     (ops[1].m_class == CLS_SNAN);
            const bool isBoth = isNanClass(ops[0].m_class) &&
                                isNanClass(ops[1].m_class);
            if (!isBoth && !(isSignaling &&
                  env.m_nanRule == FloatEnvironment::NAN_SIGNALING_FIRST))
            {
                if (isSignaling)
                    flags |= FF::FLOAT_INVALID;
                const Num& number = isNanClass(ops[0].m_class) ? ops[1]
                                                               : ops[0];
                noteDenormals(&number, 1, flags);
                return isNanClass(ops[0].m_class) ? operands[1] : operands[0];
            }
        }
        return packSpecial(fi, propagateNan(env, ops, 2, flags));
    }
    noteDenormals(ops, 2, flags);
    const int c = compareNumbers(ops[0], ops[1]);
    if (behavior == Op::BEHAVIOR_MINMAX_SECOND)
    {
        // a < b ? a : b, a > b ? a : b: equal values (and both zeros) give b
        const bool takeFirst = isMaximum ? (c > 0) : (c < 0);
        return takeFirst ? operands[0] : operands[1];
    }
    if (c == 0)
    {
        // Equal, or two zeros: -0 is the smaller
        if ((ops[0].m_class == CLS_ZERO) && (ops[0].m_sign != ops[1].m_sign))
            return (ops[0].m_sign != isMaximum) ? operands[0] : operands[1];
        return operands[0];
    }
    const bool takeFirst = isMaximum ? (c > 0) : (c < 0);
    return takeFirst ? operands[0] : operands[1];
}

/////////////////////////////////////////////////////////////////////////////
// Scale and exponent
/////////////////////////////////////////////////////////////////////////////

Bits SemanticFloat::scale(Format format, const Environment& env,
                          const Bits& a, const Bits& b, uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num ops[2] = { unpack(fi, a, env), unpack(fi, b, env) };
    const Num& x = ops[0];
    const Num& y = ops[1];
    if (hasNanOrInvalid(ops, 2))
        return packSpecial(fi, propagateNan(env, ops, 2, flags));
    noteDenormals(ops, 2, flags);
    if (y.m_class == CLS_INF)
    {
        if (!y.m_sign)
        {
            if (x.m_class == CLS_ZERO)
            {
                flags |= FF::FLOAT_INVALID;
                return packSpecial(fi, defaultNan(env));
            }
            return packInf(fi, x.m_sign);
        }
        if (x.m_class == CLS_INF)
        {
            flags |= FF::FLOAT_INVALID;
            return packSpecial(fi, defaultNan(env));
        }
        return packZero(fi, x.m_sign);
    }
    if ((x.m_class == CLS_ZERO) || (x.m_class == CLS_INF))
        return packSpecial(fi, x);
    // floor(b), clamped
    int64 n = 0;
    if (y.m_class == CLS_FINITE)
    {
        if (y.m_exp >= 24)
        {
            n = y.m_sign ? -((int64)1 << 24) : ((int64)1 << 24);
        } else
        {
            const W sig = wShl(wMake(y.m_lo, y.m_hi), 128);
            const Rounded r = roundSignificand(
                    sig, y.m_exp + 1, y.m_sign,
                    y.m_sign ? Op::ROUND_TOWARD_NEGATIVE
                             : Op::ROUND_TOWARD_ZERO);
            n = y.m_sign ? -(int64)r.m_value.w[0] : (int64)r.m_value.w[0];
        }
    }
    // The x87 precision control doesn't apply to fscale: the whole significand
    return packFinite(fi, withoutPrecision(env), x.m_sign,
                      (int32)(x.m_exp + n), x.m_hi, x.m_lo, false,
                      fi.m_precision, flags);
}

Bits SemanticFloat::getExponent(Format format, const Environment& env,
                                const Bits& a, uint32& flags)
{
    const FormatInfo fi = infoOf(format);
    const Num x = unpack(fi, a, env);
    if (hasNanOrInvalid(&x, 1))
        return packSpecial(fi, propagateNan(env, &x, 1, flags));
    noteDenormals(&x, 1, flags);
    if (x.m_class == CLS_INF)
        return packInf(fi, false);
    if (x.m_class == CLS_ZERO)
        return packInf(fi, true);
    return fromInteger(format, env, (uint64)(int64)x.m_exp, 32, true, flags);
}
