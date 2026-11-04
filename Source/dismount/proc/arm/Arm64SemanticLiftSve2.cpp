/*
 * Arm64SemanticLiftSve2.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the SVE2 integer instructions: halving, pairwise,
 * shifts by vectors and immediates, saturating and rounding arithmetic,
 * accumulates, the carry and complex integer instructions, bit permutes,
 * histograms and matches.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticInternal.h"
#include "dismount/proc/arm/Arm64SemanticSve.h"

using namespace armdis;
using namespace armdis::a64;

typedef SemanticOperation Op;

namespace {

const ArmDecodedOperand& operandOf(SveLift& s, uint index)
{
    return s.context().getOperand(index);
}

bool isPredicateAt(SveLift& s, uint index)
{
    return (index < s.context().getOperandsCount()) &&
           SveLift::isP(operandOf(s, index));
}

bool isImmediateAt(SveLift& s, uint index)
{
    return (index < s.context().getOperandsCount()) &&
           (operandOf(s, index).m_kind == ArmDecodedOperand::KIND_IMMEDIATE);
}

// The destination and the predicate of an instruction
struct Form {
    int m_predicate;
    uint m_first;
    uint m_bits;
    uint m_register;
    bool m_isZeroing;
};

Form formOf(SveLift& s)
{
    Form f;
    f.m_register = SveLift::registerNumber(operandOf(s, 0));
    f.m_bits = SveLift::elementBitsOf(operandOf(s, 0));
    f.m_predicate = -1;
    f.m_first = 1;
    f.m_isZeroing = false;
    if (isPredicateAt(s, 1))
    {
        f.m_predicate = 1;
        f.m_first = 2;
        f.m_isZeroing = operandOf(s, 1).m_predicateQualifier ==
                        ArmDecodedOperand::PREDICATE_ZEROING;
    }
    return f;
}

// Write the result: the inactive lanes keep the destination or are zero
void finish(SveLift& s, const Form& f, const SemanticOperand& computed)
{
    SemanticOperand result = computed;
    if (f.m_predicate >= 0)
        result = s.predicate(s.readMaskOperand(f.m_predicate, f.m_bits), computed,
                             s.readZ(f.m_register, f.m_bits), f.m_isZeroing);
    s.writeZ(f.m_register, result);
}

SemanticOperand K(SveLift& s, int64 value, uint e)
{
    return s.splatInteger(value, e);
}

int64 signBit(uint e)
{
    return (int64)((uint64)1 << (e - 1));
}

int64 maxSigned(uint e)
{
    return SveLift::maximumSigned(e);
}

int64 minSigned(uint e)
{
    return SveLift::minimumSigned(e);
}

// All the lanes (a mask)
SemanticOperand lanesWhere(SveLift& s, Op::Condition c, uint e, const SemanticOperand& a,
                           const SemanticOperand& b)
{
    return s.compareLanes(c, s.integerType(e), a, b);
}

// The lanes of index 2i + 'part' (0: even, 1: odd) selected by a mask
SemanticOperand parityMask(SveLift& s, uint e, uint part)
{
    SemanticOperand odd = s.emit(Op::OP_AND, s.integerType(e), s.laneIndex(0, 1, e), K(s, 1, e));
    return lanesWhere(s, Op::COND_EQUAL, e, odd, K(s, part, e));
}

// trn1 and trn2 of two vectors: the even and the odd pairs
SemanticOperand transpose(SveLift& s, uint e, const SemanticOperand& a,
                          const SemanticOperand& b, bool isSecond)
{
    const SemanticType t = s.integerType(e);
    SemanticOperand index = s.laneIndex(0, 1, e);
    SemanticOperand odd = s.emit(Op::OP_AND, t, index, K(s, 1, e));
    SemanticOperand base = isSecond ? s.emit(Op::OP_OR, t, index, K(s, 1, e))
                                    : s.emit(Op::OP_AND, t, index, K(s, ~1LL, e));
    SemanticOperand offset = s.emit(Op::OP_MULTIPLY, t, odd, K(s, s.lanes(e), e));
    return s.emit(Op::OP_SHUFFLE, t, a, b, s.emit(Op::OP_ADD, t, base, offset));
}

// Element 'index' of every 128 bit segment of the vector, in every lane of it
SemanticOperand segmentElement(SveLift& s, uint e, const SemanticOperand& vector, uint index)
{
    const uint segmentLanes = 128 / e;
    const SemanticType t = s.integerType(e);
    if (s.lanes(e) <= segmentLanes)
        return s.emit(Op::OP_SHUFFLE, t, vector, vector, K(s, index, e));
    SemanticOperand base = s.emit(Op::OP_AND, t, s.laneIndex(0, 1, e),
                                  K(s, ~(int64)(segmentLanes - 1), e));
    return s.emit(Op::OP_SHUFFLE, t, vector, vector,
                  s.emit(Op::OP_ADD, t, base, K(s, index, e)));
}

// The second source of the multiplies: a vector, or an element of the segments
SemanticOperand secondSource(SveLift& s, uint e, uint index)
{
    SemanticOperand vector = s.readZOperand(index, e);
    if (operandOf(s, index).m_elementIndex >= 0)
        return segmentElement(s, e, vector, (uint)operandOf(s, index).m_elementIndex);
    return vector;
}

/////////////////////////////////////////////////////////////////////////////
// Halving arithmetic
/////////////////////////////////////////////////////////////////////////////

enum { H_SHADD, H_UHADD, H_SRHADD, H_URHADD, H_SHSUB, H_UHSUB };

// shadd, uhadd, srhadd, urhadd, shsub, uhsub and the reversed ones. m_a: H_*,
// m_b: 1 reversed
void liftHalving(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand a = s.readZOperand(f.m_first, e);
    SemanticOperand b = s.readZOperand(f.m_first + 1, e);
    if (d.m_b != 0)
    {
        SemanticOperand x = a;
        a = b;
        b = x;
    }
    const bool isSigned = (d.m_a == H_SHADD) || (d.m_a == H_SRHADD) || (d.m_a == H_SHSUB);
    const Op::Operation shift = isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC
                                         : Op::OP_SHIFT_RIGHT_LOGICAL;
    SemanticOperand one = K(s, 1, e);
    SemanticOperand result;
    switch (d.m_a)
    {
    case H_SHADD:
    case H_UHADD:
        result = s.emit(Op::OP_ADD, t, s.emit(Op::OP_AND, t, a, b),
                        s.emit(shift, t, s.emit(Op::OP_XOR, t, a, b), one));
        break;
    case H_SRHADD:
    case H_URHADD:
        result = s.emit(Op::OP_SUBTRACT, t, s.emit(Op::OP_OR, t, a, b),
                        s.emit(shift, t, s.emit(Op::OP_XOR, t, a, b), one));
        break;
    default:
    {
        SemanticOperand borrow = s.emit(Op::OP_AND, t, s.emit(Op::OP_AND_NOT, t, a, b), one);
        result = s.emit(Op::OP_SUBTRACT, t,
                        s.emit(Op::OP_SUBTRACT, t, s.emit(shift, t, a, one), s.emit(shift, t, b, one)),
                        borrow);
        break;
    }
    }
    finish(s, f, result);
}

/////////////////////////////////////////////////////////////////////////////
// Pairwise
/////////////////////////////////////////////////////////////////////////////

// addp, subp, smaxp, umaxp, sminp, uminp. m_a: the operation
void liftPairwise(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand a = s.readZOperand(f.m_first, e);
    SemanticOperand b = s.readZOperand(f.m_first + 1, e);
    SemanticOperand x = transpose(s, e, a, b, false);
    SemanticOperand y = transpose(s, e, a, b, true);
    finish(s, f, s.emit((Op::Operation)d.m_a, t, x, y));
}

/////////////////////////////////////////////////////////////////////////////
// Shifts
/////////////////////////////////////////////////////////////////////////////

// The left shift, saturating or not
SemanticOperand shiftLeft(SveLift& s, uint e, const SemanticOperand& a,
                          const SemanticOperand& count, bool isSigned, bool isSaturating)
{
    const SemanticType t = s.integerType(e);
    SemanticOperand r = s.emit(Op::OP_SHIFT_LEFT, t, a, count);
    if (!isSaturating)
        return r;
    SemanticOperand back = s.emit(isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC
                                           : Op::OP_SHIFT_RIGHT_LOGICAL, t, r, count);
    SemanticOperand overflow = lanesWhere(s, Op::COND_NOT_EQUAL, e, back, a);
    SemanticOperand limit = K(s, -1, e);
    if (isSigned)
        limit = s.select(lanesWhere(s, Op::COND_SIGNED_LESS, e, a, K(s, 0, e)),
                         K(s, minSigned(e), e), K(s, maxSigned(e), e));
    return s.select(overflow, limit, r);
}

// The right shift, rounding or not
SemanticOperand shiftRight(SveLift& s, uint e, const SemanticOperand& a,
                           const SemanticOperand& count, bool isSigned, bool isRounding)
{
    const SemanticType t = s.integerType(e);
    if (isRounding)
        return s.emitBehavior(Op::OP_SHIFT_RIGHT_ROUNDING, t, a, count,
                              isSigned ? Op::BEHAVIOR_SIGNED : Op::BEHAVIOR_UNSIGNED);
    return s.emit(isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC : Op::OP_SHIFT_RIGHT_LOGICAL,
                  t, a, count);
}

enum { SH_ROUNDING = 1, SH_SATURATING = 2, SH_SIGNED = 4, SH_REVERSED = 8 };

// srshl, urshl, sqshl, uqshl, sqrshl, uqrshl and the reversed ones, by a
// vector (the shift amount is the signed low byte of each element) or, for
// sqshl / uqshl, by an immediate. m_a: SH_* flags
void liftShiftVector(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    const bool isSigned = (d.m_a & SH_SIGNED) != 0;
    const bool isRounding = (d.m_a & SH_ROUNDING) != 0;
    const bool isSaturating = (d.m_a & SH_SATURATING) != 0;
    SemanticOperand a = s.readZOperand(f.m_first, e);
    if (isImmediateAt(s, f.m_first + 1))
    {
        const int64 amount = operandOf(s, f.m_first + 1).m_immediate;
        finish(s, f, shiftLeft(s, e, a, K(s, amount, e), isSigned, isSaturating));
        return;
    }
    SemanticOperand b = s.readZOperand(f.m_first + 1, e);
    if ((d.m_a & SH_REVERSED) != 0)
    {
        SemanticOperand x = a;
        a = b;
        b = x;
    }
    // The shift amount is the whole element as a signed number
    SemanticOperand count = b;
    SemanticOperand isLeft = lanesWhere(s, Op::COND_SIGNED_GREATER_EQUAL, e, count, K(s, 0, e));
    SemanticOperand left = shiftLeft(s, e, a, count, isSigned, isSaturating);
    SemanticOperand right = shiftRight(s, e, a, s.emit(Op::OP_NEGATE, t, count), isSigned, isRounding);
    finish(s, f, s.select(isLeft, left, right));
}

enum { I_SRSHR, I_URSHR, I_SQSHLU, I_SSRA, I_USRA, I_SRSRA, I_URSRA, I_SRI, I_SLI };

// The shifts by an immediate: srshr, urshr, sqshlu (predicated), ssra, usra,
// srsra, ursra (accumulate), sri, sli (insert). m_a: I_*
void liftShiftImmediate(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand a = s.readZOperand(f.m_first, e);
    const int64 amount = operandOf(s, f.m_first + 1).m_immediate;
    SemanticOperand count = K(s, amount, e);
    switch (d.m_a)
    {
    case I_SRSHR:
        finish(s, f, shiftRight(s, e, a, count, true, true));
        return;
    case I_URSHR:
        finish(s, f, shiftRight(s, e, a, count, false, true));
        return;
    case I_SQSHLU:
    {
        // A signed value, an unsigned result: negative gives 0, an overflow all ones
        SemanticOperand r = s.emit(Op::OP_SHIFT_LEFT, t, a, count);
        SemanticOperand back = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, r, count);
        SemanticOperand overflow = lanesWhere(s, Op::COND_NOT_EQUAL, e, back, a);
        SemanticOperand positive = s.select(overflow, K(s, -1, e), r);
        finish(s, f, s.select(lanesWhere(s, Op::COND_SIGNED_LESS, e, a, K(s, 0, e)),
                              K(s, 0, e), positive));
        return;
    }
    case I_SSRA:
    case I_USRA:
    case I_SRSRA:
    case I_URSRA:
    {
        const bool isSigned = (d.m_a == I_SSRA) || (d.m_a == I_SRSRA);
        const bool isRounding = (d.m_a == I_SRSRA) || (d.m_a == I_URSRA);
        SemanticOperand shifted = shiftRight(s, e, a, count, isSigned, isRounding);
        finish(s, f, s.emit(Op::OP_ADD, t, s.readZ(f.m_register, e), shifted));
        return;
    }
    case I_SLI:
    {
        SemanticOperand kept = K(s, (int64)(((uint64)1 << amount) - 1), e);
        SemanticOperand destination = s.readZ(f.m_register, e);
        finish(s, f, s.emit(Op::OP_OR, t, s.emit(Op::OP_SHIFT_LEFT, t, a, count),
                            s.emit(Op::OP_AND, t, destination, kept)));
        return;
    }
    default:
    {
        // sri: the top 'amount' bits of the destination are replaced
        SemanticOperand shifted = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, a, count);
        SemanticOperand destination = s.readZ(f.m_register, e);
        SemanticOperand ones = K(s, -1, e);
        SemanticOperand kept = s.emit(Op::OP_AND_NOT, t, s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, ones, count),
                                      destination);
        finish(s, f, s.emit(Op::OP_OR, t, shifted, kept));
        return;
    }
    }
}

// sadalp, uadalp zda.T, pg/m, zn.Tb: the pairs of the narrow elements, added to zda
void liftAddAccumulateLong(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    const uint half = e / 2;
    SemanticOperand x = s.readZ(SveLift::registerNumber(operandOf(s, f.m_first)), e);
    SemanticOperand low, high;
    if (d.m_a != 0)
    {
        low = s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, t, s.emit(Op::OP_SHIFT_LEFT, t, x, K(s, half, e)),
                     K(s, half, e));
        high = s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, t, x, K(s, half, e));
    } else
    {
        low = s.emit(Op::OP_AND, t, x, K(s, (int64)(((uint64)1 << half) - 1), e));
        high = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, x, K(s, half, e));
    }
    SemanticOperand sum = s.emit(Op::OP_ADD, t, s.readZ(f.m_register, e), s.emit(Op::OP_ADD, t, low, high));
    finish(s, f, sum);
}

/////////////////////////////////////////////////////////////////////////////
// Saturating negate and absolute value, the reciprocal estimates
/////////////////////////////////////////////////////////////////////////////

// sqabs, sqneg. m_a: 1 sqabs
void liftSaturatingAbs(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand a = s.readZOperand(f.m_first, e);
    SemanticOperand negated = s.emit(Op::OP_SUBTRACT_SATURATE_SIGNED, t, K(s, 0, e), a);
    if (d.m_a != 0)
        negated = s.select(lanesWhere(s, Op::COND_SIGNED_LESS, e, a, K(s, 0, e)), negated, a);
    finish(s, f, negated);
}

// RecipEstimate of the Arm ARM for the values 256..511 (in 32 bit lanes)
SemanticOperand recipEstimate(SveLift& s, const SemanticOperand& a)
{
    const SemanticType t = s.integerType(32);
    SemanticOperand scaled = s.emit(Op::OP_ADD, t, s.emit(Op::OP_SHIFT_LEFT, t, a, K(s, 1, 32)), K(s, 1, 32));
    SemanticOperand b = s.emit(Op::OP_DIVIDE_UNSIGNED, t, K(s, 1 << 19, 32), scaled);
    return s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, s.emit(Op::OP_ADD, t, b, K(s, 1, 32)), K(s, 1, 32));
}

// RecipSqrtEstimate of the Arm ARM for the values 128..511 (in 32 bit lanes)
SemanticOperand recipSqrtEstimate(SveLift& s, const SemanticOperand& input)
{
    const SemanticType t = s.integerType(32);
    SemanticOperand isLow = lanesWhere(s, Op::COND_UNSIGNED_LESS, 32, input, K(s, 256, 32));
    SemanticOperand lowValue = s.emit(Op::OP_ADD, t, s.emit(Op::OP_SHIFT_LEFT, t, input, K(s, 1, 32)), K(s, 1, 32));
    SemanticOperand even = s.emit(Op::OP_AND, t, input, K(s, ~1LL, 32));
    SemanticOperand highValue = s.emit(Op::OP_SHIFT_LEFT, t, s.emit(Op::OP_ADD, t, even, K(s, 1, 32)), K(s, 1, 32));
    SemanticOperand a = s.select(isLow, lowValue, highValue);
    // t0: the largest value with a * t0 * t0 < 2^28 (a bit search), b = max(512, t0)
    SemanticOperand t0 = K(s, 0, 32);
    for (int bit = 10; bit >= 0; bit--)
    {
        SemanticOperand candidate = s.emit(Op::OP_OR, t, t0, K(s, 1 << bit, 32));
        SemanticOperand product = s.emit(Op::OP_MULTIPLY, t, a, s.emit(Op::OP_MULTIPLY, t, candidate, candidate));
        SemanticOperand fits = lanesWhere(s, Op::COND_UNSIGNED_LESS, 32, product, K(s, 1 << 28, 32));
        t0 = s.select(fits, candidate, t0);
    }
    SemanticOperand b = s.emit(Op::OP_MAXIMUM_UNSIGNED, t, t0, K(s, 512, 32));
    return s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, s.emit(Op::OP_ADD, t, b, K(s, 1, 32)), K(s, 1, 32));
}

// urecpe, ursqrte zd.s, pg/m, zn.s. m_a: 1 ursqrte
void liftUnsignedEstimate(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const SemanticType t = s.integerType(32);
    SemanticOperand x = s.readZOperand(f.m_first, 32);
    SemanticOperand index = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, x, K(s, 23, 32));
    SemanticOperand estimate;
    SemanticOperand isOnes;
    if (d.m_a == 0)
    {
        estimate = recipEstimate(s, s.emit(Op::OP_OR, t, index, K(s, 0, 32)));
        isOnes = lanesWhere(s, Op::COND_UNSIGNED_LESS, 32, x, K(s, (int64)0x80000000u, 32));
    } else
    {
        estimate = recipSqrtEstimate(s, index);
        isOnes = lanesWhere(s, Op::COND_UNSIGNED_LESS, 32, x, K(s, 0x40000000, 32));
    }
    SemanticOperand result = s.emit(Op::OP_SHIFT_LEFT, t,
        s.emit(Op::OP_AND, t, estimate, K(s, 0x1ff, 32)), K(s, 23, 32));
    finish(s, f, s.select(isOnes, K(s, -1, 32), result));
}

/////////////////////////////////////////////////////////////////////////////
// Saturating doubling multiplies
/////////////////////////////////////////////////////////////////////////////

// The high half of the doubled product of the signed a and b, (2 * a * b +
// rounding) >> e (or with the product negated), wrapped to e bits; 'special'
// gets the lanes of the minimum times the minimum (+2^(e-1), which wraps)
SemanticOperand doublingHigh(SveLift& s, uint e, const SemanticOperand& a, const SemanticOperand& b,
                             bool isRounding, bool isNegated, SemanticOperand* special)
{
    const SemanticType t = s.integerType(e);
    SemanticOperand lo = s.emit(Op::OP_MULTIPLY, t, a, b);
    SemanticOperand hi = s.emit(Op::OP_MULTIPLY_HIGH_SIGNED, t, a, b);
    if (special != NULL)
    {
        const SemanticType m = SemanticType::mask(s.lanes(e));
        *special = s.emit(Op::OP_AND, m, lanesWhere(s, Op::COND_EQUAL, e, a, K(s, minSigned(e), e)),
                          lanesWhere(s, Op::COND_EQUAL, e, b, K(s, minSigned(e), e)));
    }
    if (isNegated)
    {
        SemanticOperand isZero = lanesWhere(s, Op::COND_EQUAL, e, lo, K(s, 0, e));
        hi = s.emit(Op::OP_SUBTRACT, t, s.emit(Op::OP_NOT, t, hi), s.select(isZero, K(s, -1, e), K(s, 0, e)));
        lo = s.emit(Op::OP_NEGATE, t, lo);
    }
    if (isRounding)
    {
        SemanticOperand lo2 = s.emit(Op::OP_ADD, t, lo, K(s, (int64)((uint64)1 << (e - 2)), e));
        SemanticOperand carry = lanesWhere(s, Op::COND_UNSIGNED_LESS, e, lo2, lo);
        hi = s.emit(Op::OP_SUBTRACT, t, hi, s.select(carry, K(s, -1, e), K(s, 0, e)));
        lo = lo2;
    }
    return s.emit(Op::OP_OR, t, s.emit(Op::OP_SHIFT_LEFT, t, hi, K(s, 1, e)),
                  s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, lo, K(s, e - 1, e)));
}

enum { Q_DMULH, Q_RDMULH, Q_RDMLAH, Q_RDMLSH };

// The saturating doubling multiply of the vectors a and b, and the accumulate
SemanticOperand doublingMultiply(SveLift& s, uint e, uint kind, const SemanticOperand& a,
                                 const SemanticOperand& b, const SemanticOperand& accumulator)
{
    const SemanticType t = s.integerType(e);
    SemanticOperand special;
    const bool isRounding = kind != Q_DMULH;
    SemanticOperand m = doublingHigh(s, e, a, b, isRounding, kind == Q_RDMLSH, &special);
    if (kind == Q_DMULH || kind == Q_RDMULH)
        return s.select(special, K(s, maxSigned(e), e), m);
    SemanticOperand sum = s.emit(Op::OP_ADD_SATURATE_SIGNED, t, accumulator, m);
    if (kind == Q_RDMLSH)
        return sum;
    // The minimum times the minimum is +2^(e-1)
    SemanticOperand wrapped = s.select(lanesWhere(s, Op::COND_SIGNED_GREATER_EQUAL, e, accumulator, K(s, 0, e)),
                                       K(s, maxSigned(e), e),
                                       s.emit(Op::OP_XOR, t, accumulator, K(s, signBit(e), e)));
    return s.select(special, wrapped, sum);
}

// sqdmulh, sqrdmulh, sqrdmlah, sqrdmlsh (vectors and by element). m_a: Q_*
void liftDoublingMultiply(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    SemanticOperand a = s.readZOperand(f.m_first, e);
    SemanticOperand b = secondSource(s, e, f.m_first + 1);
    SemanticOperand accumulator = s.readZ(f.m_register, e);
    finish(s, f, doublingMultiply(s, e, d.m_a, a, b, accumulator));
}

/////////////////////////////////////////////////////////////////////////////
// Accumulating absolute difference, clamp, bitwise
/////////////////////////////////////////////////////////////////////////////

// saba, uaba zda, zn, zm. m_a: the difference operation
void liftAbsoluteAccumulate(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand diff = s.emit((Op::Operation)d.m_a, t, s.readZOperand(1, e), s.readZOperand(2, e));
    s.writeZ(f.m_register, s.emit(Op::OP_ADD, t, s.readZ(f.m_register, e), diff));
}

// sclamp, uclamp zd, zn, zm: min(max(zd, zn), zm)
void liftClamp(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    const bool isSigned = d.m_a != 0;
    SemanticOperand r = s.emit(isSigned ? Op::OP_MAXIMUM_SIGNED : Op::OP_MAXIMUM_UNSIGNED, t,
                               s.readZ(f.m_register, e), s.readZOperand(1, e));
    s.writeZ(f.m_register, s.emit(isSigned ? Op::OP_MINIMUM_SIGNED : Op::OP_MINIMUM_UNSIGNED, t,
                                  r, s.readZOperand(2, e)));
}

// eor3, bcax, bsl, bsl1n, bsl2n, nbsl, xar. m_a: 0 eor3, 1 bcax, 2 bsl, 3 bsl1n, 4 bsl2n,
// 5 nbsl, 6 xar
void liftBitwise(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand dn = s.readZOperand(1, e);
    SemanticOperand m = s.readZOperand(2, e);
    SemanticOperand result;
    if (d.m_a == 6)
    {
        const int64 amount = operandOf(s, 3).m_immediate;
        result = s.emit(Op::OP_ROTATE_RIGHT, t, s.emit(Op::OP_XOR, t, dn, m), K(s, amount, e));
        s.writeZ(f.m_register, result);
        return;
    }
    SemanticOperand k = s.readZOperand(3, e);
    switch (d.m_a)
    {
    case 0: result = s.emit(Op::OP_XOR, t, s.emit(Op::OP_XOR, t, dn, m), k); break;
    case 1: result = s.emit(Op::OP_XOR, t, dn, s.emit(Op::OP_AND_NOT, t, k, m)); break;
    case 2: result = s.emit(Op::OP_BITWISE_SELECT, t, k, dn, m); break;
    case 3: result = s.emit(Op::OP_BITWISE_SELECT, t, k, s.emit(Op::OP_NOT, t, dn), m); break;
    case 4: result = s.emit(Op::OP_BITWISE_SELECT, t, k, dn, s.emit(Op::OP_NOT, t, m)); break;
    default:
        result = s.emit(Op::OP_NOT, t, s.emit(Op::OP_BITWISE_SELECT, t, k, dn, m));
        break;
    }
    s.writeZ(f.m_register, result);
}

// eorbt, eortb zd, zn, zm: the even (odd) lanes get zn ^ the odd (even) lane of zm. m_a: 1 eortb
void liftInterleavedEor(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand n = s.readZOperand(1, e);
    SemanticOperand m = s.readZOperand(2, e);
    SemanticOperand moved = transpose(s, e, m, m, d.m_a == 0);
    SemanticOperand x = s.emit(Op::OP_XOR, t, n, moved);
    s.writeZ(f.m_register, s.select(parityMask(s, e, d.m_a), x, s.readZ(f.m_register, e)));
}

// pmul zd.b, zn.b, zm.b
void liftPolynomialMultiply(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    s.writeZ(f.m_register, s.emit(Op::OP_CARRYLESS_MULTIPLY, s.integerType(8),
                                  s.readZOperand(1, 8), s.readZOperand(2, 8)));
}

/////////////////////////////////////////////////////////////////////////////
// Complex integers
/////////////////////////////////////////////////////////////////////////////

// cadd, sqcadd zdn, zdn, zm, #90|#270. m_a: 1 sqcadd
void liftComplexAdd(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand a = s.readZOperand(1, e);
    SemanticOperand b = s.readZOperand(2, e);
    const bool is270 = operandOf(s, 3).m_immediate == 270;
    SemanticOperand index = s.laneIndex(0, 1, e);
    SemanticOperand swapped = s.emit(Op::OP_SHUFFLE, t, b, b, s.emit(Op::OP_XOR, t, index, K(s, 1, e)));
    const Op::Operation add = (d.m_a != 0) ? Op::OP_ADD_SATURATE_SIGNED : Op::OP_ADD;
    const Op::Operation sub = (d.m_a != 0) ? Op::OP_SUBTRACT_SATURATE_SIGNED : Op::OP_SUBTRACT;
    SemanticOperand plus = s.emit(add, t, a, swapped);
    SemanticOperand minus = s.emit(sub, t, a, swapped);
    // rot90: the real lanes subtract; rot270: the imaginary lanes subtract
    SemanticOperand subtractLanes = parityMask(s, e, is270 ? 1 : 0);
    s.writeZ(f.m_register, s.select(subtractLanes, minus, plus));
}

// The signs of the complex multiply: the lanes whose product is negated
SemanticOperand negatedLanes(SveLift& s, uint e, uint rotation)
{
    if (rotation == 0)
        return s.zero(SemanticType::mask(s.lanes(e)));
    if (rotation == 2)
        return s.bitcast(s.integer(-1, s.lanes(e)), SemanticType::mask(s.lanes(e)));
    return parityMask(s, e, rotation == 1 ? 0 : 1);
}

// cmla, sqrdcmlah zda, zn, zm, #rot | zda, zn, zm[i], #rot. m_a: 1 sqrdcmlah
void liftComplexMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType t = s.integerType(e);
    SemanticOperand n = s.readZOperand(1, e);
    SemanticOperand m;
    if (operandOf(s, 2).m_elementIndex >= 0)
    {
        // The complex element of every 128 bit segment
        SemanticOperand pairs = s.readZOperand(2, 2 * e);
        const uint pairsPerSegment = 128 / (2 * e);
        SemanticOperand lanes = s.laneIndex(0, 1, 2 * e);
        SemanticOperand base = s.emit(Op::OP_AND, s.integerType(2 * e), lanes,
                                      K(s, ~(int64)(pairsPerSegment - 1), 2 * e));
        SemanticOperand selector = s.emit(Op::OP_ADD, s.integerType(2 * e), base,
                                          K(s, operandOf(s, 2).m_elementIndex, 2 * e));
        m = s.bitcast(s.emit(Op::OP_SHUFFLE, s.integerType(2 * e), pairs, pairs, selector), t);
    } else
    {
        m = s.readZOperand(2, e);
    }
    const uint rotation = (uint)(operandOf(s, 3).m_immediate / 90) & 3;
    SemanticOperand accumulator = s.readZ(f.m_register, e);
    const uint a = rotation & 1;
    SemanticOperand index = s.laneIndex(0, 1, e);
    SemanticOperand pairBase = s.emit(Op::OP_AND, t, index, K(s, ~1LL, e));
    SemanticOperand odd = s.emit(Op::OP_AND, t, index, K(s, 1, e));
    SemanticOperand nSelector = s.emit(Op::OP_OR, t, pairBase, K(s, a, e));
    SemanticOperand mSelector = s.emit(Op::OP_OR, t, pairBase, s.emit(Op::OP_XOR, t, odd, K(s, a, e)));
    SemanticOperand nSelected = s.emit(Op::OP_SHUFFLE, t, n, n, nSelector);
    SemanticOperand mSelected = s.emit(Op::OP_SHUFFLE, t, m, m, mSelector);
    SemanticOperand negated = negatedLanes(s, e, rotation);
    SemanticOperand result;
    if (d.m_a == 0)
    {
        SemanticOperand product = s.emit(Op::OP_MULTIPLY, t, nSelected, mSelected);
        result = s.select(negated, s.emit(Op::OP_SUBTRACT, t, accumulator, product),
                          s.emit(Op::OP_ADD, t, accumulator, product));
    } else
    {
        result = s.select(negated, doublingMultiply(s, e, Q_RDMLSH, nSelected, mSelected, accumulator),
                          doublingMultiply(s, e, Q_RDMLAH, nSelected, mSelected, accumulator));
    }
    s.writeZ(f.m_register, result);
}

// cdot zda.T, zn.Tb, zm.Tb, #rot | zda.T, zn.Tb, zm.Tb[i], #rot: each element of zda gets
// the real or imaginary sums of two complex products of the four elements of zn and zm
void liftComplexDot(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const uint n = e / 4;
    const SemanticType wide = s.integerType(e);
    SemanticOperand vn = s.readZOperand(1, e);
    SemanticOperand vm = s.readZOperand(2, e);
    if (operandOf(s, 2).m_elementIndex >= 0)
        vm = segmentElement(s, e, vm, (uint)operandOf(s, 2).m_elementIndex);
    SemanticOperand accumulator = s.readZ(f.m_register, e);
    const uint rotation = (uint)(operandOf(s, 3).m_immediate / 90) & 3;
    SemanticOperand result = accumulator;
    for (uint lane = 0; lane < s.lanes(e); lane++)
    {
        SemanticOperand sum = s.extractLane(accumulator, lane);
        SemanticOperand xn = s.extractLane(vn, lane);
        SemanticOperand xm = s.extractLane(vm, lane);
        for (uint pair = 0; pair < 2; pair++)
        {
            SemanticOperand nr = s.emit(Op::OP_SIGN_EXTEND, SemanticType::integer(e),
                s.emit(Op::OP_EXTRACT, SemanticType::integer(n), xn, s.integer(pair * 2 * n, 32)));
            SemanticOperand ni = s.emit(Op::OP_SIGN_EXTEND, SemanticType::integer(e),
                s.emit(Op::OP_EXTRACT, SemanticType::integer(n), xn, s.integer((pair * 2 + 1) * n, 32)));
            SemanticOperand mr = s.emit(Op::OP_SIGN_EXTEND, SemanticType::integer(e),
                s.emit(Op::OP_EXTRACT, SemanticType::integer(n), xm, s.integer(pair * 2 * n, 32)));
            SemanticOperand mi = s.emit(Op::OP_SIGN_EXTEND, SemanticType::integer(e),
                s.emit(Op::OP_EXTRACT, SemanticType::integer(n), xm, s.integer((pair * 2 + 1) * n, 32)));
            const SemanticType scalar = SemanticType::integer(e);
            SemanticOperand rr = s.emit(Op::OP_MULTIPLY, scalar, nr, mr);
            SemanticOperand ii = s.emit(Op::OP_MULTIPLY, scalar, ni, mi);
            SemanticOperand ri = s.emit(Op::OP_MULTIPLY, scalar, nr, mi);
            SemanticOperand ir = s.emit(Op::OP_MULTIPLY, scalar, ni, mr);
            switch (rotation)
            {
            case 0: sum = s.emit(Op::OP_SUBTRACT, scalar, s.emit(Op::OP_ADD, scalar, sum, rr), ii); break;
            case 1: sum = s.emit(Op::OP_ADD, scalar, s.emit(Op::OP_ADD, scalar, sum, ri), ir); break;
            case 2: sum = s.emit(Op::OP_ADD, scalar, s.emit(Op::OP_ADD, scalar, sum, rr), ii); break;
            default: sum = s.emit(Op::OP_SUBTRACT, scalar, s.emit(Op::OP_ADD, scalar, sum, ri), ir); break;
            }
        }
        result = s.insertLane(result, sum, lane);
    }
    (void)wide;
    s.writeZ(f.m_register, result);
}

// adclb, adclt, sbclb, sbclt zda, zn, zm. m_a: 1 top, m_b: 1 subtract
void liftCarryLong(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType scalar = SemanticType::integer(e);
    SemanticOperand n = s.readZOperand(1, e);
    SemanticOperand m = s.readZOperand(2, e);
    SemanticOperand a = s.readZ(f.m_register, e);
    SemanticOperand result = a;
    for (uint pair = 0; pair < s.lanes(e) / 2; pair++)
    {
        // zda[even] + zn[even or odd] (inverted to subtract) + the carry of zm[odd]
        SemanticOperand x = s.extractLane(a, 2 * pair);
        SemanticOperand y = s.extractLane(n, 2 * pair + d.m_a);
        if (d.m_b != 0)
            y = s.emit(Op::OP_NOT, scalar, y);
        SemanticOperand carry = s.emit(Op::OP_AND, scalar, s.extractLane(m, 2 * pair + 1), s.integer(1, e));
        SemanticOperand sum = s.emit(Op::OP_ADD, scalar, s.emit(Op::OP_ADD, scalar, x, y), carry);
        // the carry out: the top bit of (x & y) | ((x | y) & ~sum)
        SemanticOperand out = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, scalar,
            s.emit(Op::OP_OR, scalar, s.emit(Op::OP_AND, scalar, x, y),
                   s.emit(Op::OP_AND_NOT, scalar, sum, s.emit(Op::OP_OR, scalar, x, y))),
            s.integer(e - 1, e));
        result = s.insertLane(result, sum, 2 * pair);
        result = s.insertLane(result, out, 2 * pair + 1);
    }
    s.writeZ(f.m_register, result);
}

/////////////////////////////////////////////////////////////////////////////
// Bit permutes
/////////////////////////////////////////////////////////////////////////////

// bext, bdep, bgrp zd, zn, zm. m_a: 0 bext, 1 bdep, 2 bgrp
void liftBitPermute(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const SemanticType scalar = SemanticType::integer(e);
    SemanticOperand n = s.readZOperand(1, e);
    SemanticOperand m = s.readZOperand(2, e);
    SemanticOperand result = s.zero(s.integerType(e));
    for (uint lane = 0; lane < s.lanes(e); lane++)
    {
        SemanticOperand x = s.extractLane(n, lane);
        SemanticOperand mask = s.extractLane(m, lane);
        SemanticOperand value;
        if (d.m_a == 0)
            value = s.emit(Op::OP_BIT_GATHER, scalar, x, mask);
        else if (d.m_a == 1)
            value = s.emit(Op::OP_BIT_DEPOSIT, scalar, x, mask);
        else
        {
            SemanticOperand low = s.emit(Op::OP_BIT_GATHER, scalar, x, mask);
            SemanticOperand notMask = s.emit(Op::OP_NOT, scalar, mask);
            SemanticOperand high = s.emit(Op::OP_BIT_GATHER, scalar, x, notMask);
            SemanticOperand count = s.emit(Op::OP_POPULATION_COUNT, scalar, mask);
            value = s.emit(Op::OP_OR, scalar, low, s.emit(Op::OP_SHIFT_LEFT, scalar, high, count));
        }
        result = s.insertLane(result, value, lane);
    }
    s.writeZ(f.m_register, result);
}

/////////////////////////////////////////////////////////////////////////////
// Histograms and matches
/////////////////////////////////////////////////////////////////////////////

// The bit 'lane' of the lane mask bits as one bit
SemanticOperand bitOf(SveLift& s, const SemanticOperand& bits, uint count, uint lane)
{
    const SemanticType type = SemanticType::integer(count);
    return s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                  s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, bits, s.integer(lane, count)));
}

// histcnt zd.T, pg/z, zn.T, zm.T: the number of earlier (and this) active elements of zn
// equal to each active element of zm
void liftHistogramCount(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint e = f.m_bits;
    const uint count = s.lanes(e);
    SemanticOperand maskBits = s.bitcast(s.readMaskOperand(1, e), SemanticType::integer(count));
    SemanticOperand n = s.readZOperand(2, e);
    SemanticOperand m = s.readZOperand(3, e);
    SemanticOperand result = s.zero(s.integerType(e));
    const SemanticType scalar = SemanticType::integer(e);
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand total = s.integer(0, e);
        SemanticOperand ml = s.extractLane(m, lane);
        for (uint i = 0; i <= lane; i++)
        {
            SemanticOperand equal = s.compareScalar(Op::COND_EQUAL, s.extractLane(n, i), ml);
            SemanticOperand hit = s.emit(Op::OP_AND, SemanticType::integer(1), equal, bitOf(s, maskBits, count, i));
            total = s.emit(Op::OP_ADD, scalar, total, s.emit(Op::OP_ZERO_EXTEND, scalar, hit));
        }
        SemanticOperand value = s.select(bitOf(s, maskBits, count, lane), total, s.integer(0, e));
        result = s.insertLane(result, value, lane);
    }
    s.writeZ(f.m_register, result);
}

// histseg zd.b, zn.b, zm.b: in each 128 bit segment, the number of elements of zn equal to
// each element of zm
void liftHistogramSegment(SveLift& s, const SveDescriptor& d)
{
    const Form f = formOf(s);
    const uint count = s.lanes(8);
    SemanticOperand n = s.readZOperand(1, 8);
    SemanticOperand m = s.readZOperand(2, 8);
    SemanticOperand result = s.zero(s.integerType(8));
    const SemanticType scalar = SemanticType::integer(8);
    for (uint lane = 0; lane < count; lane++)
    {
        const uint segment = lane / 16;
        SemanticOperand total = s.integer(0, 8);
        SemanticOperand ml = s.extractLane(m, lane);
        for (uint i = 0; i < 16; i++)
        {
            SemanticOperand equal = s.compareScalar(Op::COND_EQUAL, s.extractLane(n, segment * 16 + i), ml);
            total = s.emit(Op::OP_ADD, scalar, total, s.emit(Op::OP_ZERO_EXTEND, scalar, equal));
        }
        result = s.insertLane(result, total, lane);
    }
    s.writeZ(f.m_register, result);
}

// match, nmatch pd.T, pg/z, zn.T, zm.T: each active element of zn against the elements of
// its 128 bit segment of zm. m_a: 1 nmatch
void liftMatch(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint count = s.lanes(e);
    const uint segmentLanes = 128 / e;
    SemanticOperand governing = s.readPredicateBits(SveLift::registerNumber(operandOf(s, 1)));
    SemanticOperand maskBits = s.bitcast(s.readMaskOperand(1, e), SemanticType::integer(count));
    SemanticOperand n = s.readZOperand(2, e);
    SemanticOperand m = s.readZOperand(3, e);
    SemanticOperand bits = s.integer(0, count);
    for (uint lane = 0; lane < count; lane++)
    {
        const uint segment = lane / segmentLanes;
        SemanticOperand x = s.extractLane(n, lane);
        SemanticOperand found = s.integer(0, 1);
        for (uint i = 0; i < segmentLanes; i++)
            found = s.emit(Op::OP_OR, SemanticType::integer(1), found,
                           s.compareScalar(Op::COND_EQUAL, x, s.extractLane(m, segment * segmentLanes + i)));
        if (d.m_a != 0)
            found = s.emit(Op::OP_XOR, SemanticType::integer(1), found, s.integer(1, 1));
        SemanticOperand hit = s.emit(Op::OP_AND, SemanticType::integer(1), found, bitOf(s, maskBits, count, lane));
        SemanticOperand placed = s.emit(Op::OP_SHIFT_LEFT, SemanticType::integer(count),
            s.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(count), hit), s.integer(lane, count));
        bits = s.emit(Op::OP_OR, SemanticType::integer(count), bits, placed);
    }
    SemanticOperand predicate = s.maskToPredicate(s.bitcast(bits, SemanticType::mask(count)), e);
    s.writePredicateBits(SveLift::registerNumber(operandOf(s, 0)), predicate);
    s.setPredicateFlags(governing, predicate, e);
}

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

const SveDescriptor gTable[] =
{
    { "shadd", liftHalving, H_SHADD, 0 }, { "uhadd", liftHalving, H_UHADD, 0 },
    { "srhadd", liftHalving, H_SRHADD, 0 }, { "urhadd", liftHalving, H_URHADD, 0 },
    { "shsub", liftHalving, H_SHSUB, 0 }, { "uhsub", liftHalving, H_UHSUB, 0 },
    { "shsubr", liftHalving, H_SHSUB, 1 }, { "uhsubr", liftHalving, H_UHSUB, 1 },
    { "addp", liftPairwise, Op::OP_ADD, 0 }, { "subp", liftPairwise, Op::OP_SUBTRACT, 0 },
    { "smaxp", liftPairwise, Op::OP_MAXIMUM_SIGNED, 0 }, { "umaxp", liftPairwise, Op::OP_MAXIMUM_UNSIGNED, 0 },
    { "sminp", liftPairwise, Op::OP_MINIMUM_SIGNED, 0 }, { "uminp", liftPairwise, Op::OP_MINIMUM_UNSIGNED, 0 },
    { "srshl", liftShiftVector, SH_ROUNDING | SH_SIGNED, 0 },
    { "urshl", liftShiftVector, SH_ROUNDING, 0 },
    { "srshlr", liftShiftVector, SH_ROUNDING | SH_SIGNED | SH_REVERSED, 0 },
    { "urshlr", liftShiftVector, SH_ROUNDING | SH_REVERSED, 0 },
    { "sqshl", liftShiftVector, SH_SATURATING | SH_SIGNED, 0 },
    { "uqshl", liftShiftVector, SH_SATURATING, 0 },
    { "sqrshl", liftShiftVector, SH_SATURATING | SH_ROUNDING | SH_SIGNED, 0 },
    { "uqrshl", liftShiftVector, SH_SATURATING | SH_ROUNDING, 0 },
    { "sqshlr", liftShiftVector, SH_SATURATING | SH_SIGNED | SH_REVERSED, 0 },
    { "uqshlr", liftShiftVector, SH_SATURATING | SH_REVERSED, 0 },
    { "sqrshlr", liftShiftVector, SH_SATURATING | SH_ROUNDING | SH_SIGNED | SH_REVERSED, 0 },
    { "uqrshlr", liftShiftVector, SH_SATURATING | SH_ROUNDING | SH_REVERSED, 0 },
    { "srshr", liftShiftImmediate, I_SRSHR, 0 }, { "urshr", liftShiftImmediate, I_URSHR, 0 },
    { "sqshlu", liftShiftImmediate, I_SQSHLU, 0 },
    { "ssra", liftShiftImmediate, I_SSRA, 0 }, { "usra", liftShiftImmediate, I_USRA, 0 },
    { "srsra", liftShiftImmediate, I_SRSRA, 0 }, { "ursra", liftShiftImmediate, I_URSRA, 0 },
    { "sri", liftShiftImmediate, I_SRI, 0 }, { "sli", liftShiftImmediate, I_SLI, 0 },
    { "sadalp", liftAddAccumulateLong, 1, 0 }, { "uadalp", liftAddAccumulateLong, 0, 0 },
    { "sqabs", liftSaturatingAbs, 1, 0 }, { "sqneg", liftSaturatingAbs, 0, 0 },
    { "urecpe", liftUnsignedEstimate, 0, 0 }, { "ursqrte", liftUnsignedEstimate, 1, 0 },
    { "sqdmulh", liftDoublingMultiply, Q_DMULH, 0 }, { "sqrdmulh", liftDoublingMultiply, Q_RDMULH, 0 },
    { "sqrdmlah", liftDoublingMultiply, Q_RDMLAH, 0 }, { "sqrdmlsh", liftDoublingMultiply, Q_RDMLSH, 0 },
    { "saba", liftAbsoluteAccumulate, Op::OP_ABSOLUTE_DIFFERENCE_SIGNED, 0 },
    { "uaba", liftAbsoluteAccumulate, Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED, 0 },
    { "sclamp", liftClamp, 1, 0 }, { "uclamp", liftClamp, 0, 0 },
    { "histcnt", liftHistogramCount, 0, 0 }, { "histseg", liftHistogramSegment, 0, 0 },
    { "match", liftMatch, 0, 0 }, { "nmatch", liftMatch, 1, 0 },
    { "eorbt", liftInterleavedEor, 0, 0 }, { "eortb", liftInterleavedEor, 1, 0 },
    { "cadd", liftComplexAdd, 0, 0 }, { "sqcadd", liftComplexAdd, 1, 0 },
    { "adclb", liftCarryLong, 0, 0 }, { "adclt", liftCarryLong, 1, 0 },
    { "sbclb", liftCarryLong, 0, 1 }, { "sbclt", liftCarryLong, 1, 1 },
    { "cmla", liftComplexMultiplyAdd, 0, 0 }, { "sqrdcmlah", liftComplexMultiplyAdd, 1, 0 },
    { "cdot", liftComplexDot, 0, 0 },
    { "bext", liftBitPermute, 0, 0 }, { "bdep", liftBitPermute, 1, 0 }, { "bgrp", liftBitPermute, 2, 0 },
    { "eor3", liftBitwise, 0, 0 }, { "bcax", liftBitwise, 1, 0 }, { "bsl", liftBitwise, 2, 0 },
    { "bsl1n", liftBitwise, 3, 0 }, { "bsl2n", liftBitwise, 4, 0 }, { "nbsl", liftBitwise, 5, 0 },
    { "xar", liftBitwise, 6, 0 },
    { "pmul", liftPolynomialMultiply, 0, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSve2(ArmLiftContext& context)
{
    runSveTable(context, gTable);
}
