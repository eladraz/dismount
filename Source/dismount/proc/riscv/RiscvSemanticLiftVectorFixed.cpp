/*
 * RiscvSemanticLiftVectorFixed.cpp
 *
 * Implementation file
 *
 * The vector fixed point instructions: the saturating add and subtract, the
 * averaging add and subtract (vxrm rounding), vsmul, the scaling shifts
 * vssrl / vssra and the narrowing clips vnclip / vnclipu. The rounding mode
 * is vxrm (rnu, rne, rdn, rod) read at runtime, vxsat is set when a lane
 * saturates (only the active lanes count).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"
#include "dismount/proc/riscv/RiscvSemanticVector.h"

namespace riscvdis {

typedef SemanticOperation Op;

namespace {

// a +/- b saturated, and the lanes that saturated
SemanticOperand saturating(RvvChunk& c, Op::Operation saturated,
                           Op::Operation wrapped)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    SemanticOperand result = l.emit(saturated, t, c.m_a, c.m_b);
    SemanticOperand plain = l.emit(wrapped, t, c.m_a, c.m_b);
    c.m_saturated = l.compare(Op::COND_NOT_EQUAL, t, result, plain);
    return result;
}

// The averaging add and subtract: the floor of the sum / difference over 2
// plus the rounding increment of the bit that was shifted out
SemanticOperand averaging(RvvChunk& c, bool isSigned, bool isSubtract)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    const Op::Operation shift = isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC :
                                           Op::OP_SHIFT_RIGHT_LOGICAL;
    SemanticOperand one = l.splat(1, t);
    SemanticOperand a1 = l.emit(shift, t, c.m_a, one);
    SemanticOperand b1 = l.emit(shift, t, c.m_b, one);
    SemanticOperand floor;
    if (!isSubtract)
    {
        // (a >> 1) + (b >> 1) + (a & b & 1)
        SemanticOperand carry = l.emit(Op::OP_AND, t,
                                       l.emit(Op::OP_AND, t, c.m_a, c.m_b), one);
        floor = l.emit(Op::OP_ADD, t, l.emit(Op::OP_ADD, t, a1, b1), carry);
    } else
    {
        // (a >> 1) - (b >> 1) - (~a & b & 1)
        SemanticOperand borrow = l.emit(Op::OP_AND, t,
                                        l.emit(Op::OP_AND_NOT, t, c.m_a, c.m_b),
                                        one);
        floor = l.emit(Op::OP_SUBTRACT, t, l.emit(Op::OP_SUBTRACT, t, a1, b1),
                       borrow);
    }
    // The bit that was shifted out is the low bit of the sum / difference,
    // a ^ b
    SemanticOperand parity = l.emit(Op::OP_XOR, t, c.m_a, c.m_b);
    SemanticOperand increment = l.roundingIncrement(c, parity, one, floor);
    return l.emit(Op::OP_ADD, t, floor, increment);
}

// vsmul: (a * b) >> (bits - 1) rounded, saturated to the signed range
SemanticOperand fractionalMultiply(RvvChunk& c)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    const uint bits = c.m_bits;
    SemanticOperand high = l.emit(Op::OP_MULTIPLY_HIGH_SIGNED, t, c.m_a, c.m_b);
    SemanticOperand low = l.emit(Op::OP_MULTIPLY, t, c.m_a, c.m_b);
    // The product shifted right by bits - 1: high:low >> (bits - 1)
    SemanticOperand upper = l.emit(Op::OP_SHIFT_LEFT, t, high, l.splat(1, t));
    SemanticOperand lower = l.emit(Op::OP_SHIFT_RIGHT_LOGICAL, t, low,
                                   l.splat(bits - 1, t));
    SemanticOperand shifted = l.emit(Op::OP_OR, t, upper, lower);
    SemanticOperand increment = l.roundingIncrement(c, low,
                                                    l.splat(bits - 1, t), shifted);
    SemanticOperand result = l.emit(Op::OP_ADD, t, shifted, increment);
    // Only the minimum * the minimum overflows
    const int64 minimum = (int64)((uint64)1 << (bits - 1));
    SemanticOperand isOverflow = l.emit(Op::OP_AND, c.m_maskType,
            l.compare(Op::COND_EQUAL, t, c.m_a, l.splat(minimum, t)),
            l.compare(Op::COND_EQUAL, t, c.m_b, l.splat(minimum, t)));
    c.m_saturated = isOverflow;
    const int64 maximum = (int64)(((uint64)1 << (bits - 1)) - 1);
    return l.select(isOverflow, l.splat(maximum, t), result);
}

// vssrl / vssra
SemanticOperand scalingShift(RvvChunk& c, bool isSigned)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    SemanticOperand count = l.emit(Op::OP_AND, t, c.m_b, l.splat(c.m_bits - 1, t));
    SemanticOperand shifted = l.emit(isSigned ? Op::OP_SHIFT_RIGHT_ARITHMETIC :
                                     Op::OP_SHIFT_RIGHT_LOGICAL, t, c.m_a, count);
    SemanticOperand increment = l.roundingIncrement(c, c.m_a, count, shifted);
    return l.emit(Op::OP_ADD, t, shifted, increment);
}

// vnclip / vnclipu: the wide vs2 shifted with rounding, clipped to the
// narrow range. The lanes are the wide ones
SemanticOperand narrowingClip(RvvChunk& c, bool isSigned)
{
    RvvLift& l = c.m_lift;
    const SemanticType t = c.m_vectorType;
    const uint narrow = c.m_bits / 2;
    SemanticOperand result = scalingShift(c, isSigned);
    if (isSigned)
    {
        const int64 maximum = (int64)(((uint64)1 << (narrow - 1)) - 1);
        const int64 minimum = -maximum - 1;
        SemanticOperand high = l.compare(Op::COND_SIGNED_GREATER, t, result,
                                         l.splat(maximum, t));
        SemanticOperand low = l.compare(Op::COND_SIGNED_LESS, t, result,
                                        l.splat(minimum, t));
        c.m_saturated = l.emit(Op::OP_OR, c.m_maskType, high, low);
        result = l.select(high, l.splat(maximum, t), result);
        return l.select(low, l.splat(minimum, t), result);
    }
    const int64 maximum = (int64)(((uint64)1 << narrow) - 1);
    SemanticOperand high = l.compare(Op::COND_UNSIGNED_GREATER, t, result,
                                     l.splat(maximum, t));
    c.m_saturated = high;
    return l.select(high, l.splat(maximum, t), result);
}

} // namespace

SemanticOperand fixedElementOp(RvvChunk& c)
{
    switch (c.m_plan.m_operation)
    {
    case RVV_OP_SADDU:
        return saturating(c, Op::OP_ADD_SATURATE_UNSIGNED, Op::OP_ADD);
    case RVV_OP_SADD:
        return saturating(c, Op::OP_ADD_SATURATE_SIGNED, Op::OP_ADD);
    case RVV_OP_SSUBU:
        return saturating(c, Op::OP_SUBTRACT_SATURATE_UNSIGNED, Op::OP_SUBTRACT);
    case RVV_OP_SSUB:
        return saturating(c, Op::OP_SUBTRACT_SATURATE_SIGNED, Op::OP_SUBTRACT);
    case RVV_OP_AADDU:
        return averaging(c, false, false);
    case RVV_OP_AADD:
        return averaging(c, true, false);
    case RVV_OP_ASUBU:
        return averaging(c, false, true);
    case RVV_OP_ASUB:
        return averaging(c, true, true);
    case RVV_OP_SMUL:
        return fractionalMultiply(c);
    case RVV_OP_SSRL:
        return scalingShift(c, false);
    case RVV_OP_SSRA:
        return scalingShift(c, true);
    case RVV_OP_NCLIPU:
        return narrowingClip(c, false);
    case RVV_OP_NCLIP:
        return narrowingClip(c, true);
    default:
        CHECK_FAIL();
    }
}

} // namespace riscvdis
