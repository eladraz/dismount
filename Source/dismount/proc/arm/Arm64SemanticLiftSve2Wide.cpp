/*
 * Arm64SemanticLiftSve2Wide.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the SVE2 widening and narrowing instructions: the
 * long and wide arithmetic on the even (bottom) and odd (top) elements, the
 * multiply accumulate long, the narrowing moves and shifts (also the two
 * vector forms of SVE2.1), the high narrowing add and subtract, the polynomial
 * multiplies, the shifts long, the dot products, the integer matrix
 * multiplies and adr.
 *
 * A wide element of 'e' bits is the lane of the operation: the bottom
 * narrow element of a source is its low e / 2 bits and the top one its high
 * bits, so the operands are extracted with whole lane operations.
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

uint regOf(SveLift& s, uint index)
{
    return SveLift::registerNumber(operandOf(s, index));
}

// The flags of the descriptors (m_b)
enum {
    // The top (odd) elements
    F_TOP = 1,
    // Unsigned elements
    F_UNSIGNED = 2,
    // The first operand uses the bottom and the second the top element
    F_BOTTOM_TOP = 4,
    // The first operand uses the top and the second the bottom element
    F_TOP_BOTTOM = 8,
    // The first operand is wide
    F_WIDE = 16
};

// The bottom or top narrow element of every wide lane, extended to the lane
SemanticOperand narrowElement(SveLift& s, const SemanticOperand& x, uint e,
                              bool isTop, bool isSigned)
{
    const SemanticType type = s.integerType(e);
    const uint n = e / 2;
    if (!isSigned)
    {
        if (isTop)
            return s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, x, s.splatInteger(n, e));
        return s.emit(Op::OP_AND, type, x, s.splatInteger((int64)(((uint64)1 << n) - 1), e));
    }
    if (isTop)
        return s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type, x, s.splatInteger(n, e));
    return s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, type,
                  s.emit(Op::OP_SHIFT_LEFT, type, x, s.splatInteger(n, e)),
                  s.splatInteger(n, e));
}

// The second source of the long instructions: a vector, or the element
// 'z.h[i]' of every 128 bit segment replicated in the narrow lanes
SemanticOperand secondSource(SveLift& s, uint index, uint e)
{
    const ArmDecodedOperand& operand = operandOf(s, index);
    if (operand.m_elementIndex < 0)
        return s.readZ(regOf(s, index), e);
    const uint n = e / 2;
    const uint lanes = s.lanes(n);
    const SemanticType narrowType = s.integerType(n);
    const SemanticType indexType = SemanticType::vector(SemanticType::integer(16), lanes);
    SemanticOperand narrow = s.readZ(regOf(s, index), n);
    SemanticOperand lane = s.laneIndexWide(0, 1, lanes, 16);
    SemanticOperand base = s.emit(Op::OP_AND, indexType, lane,
        s.splat(s.integer(~(int64)(128 / n - 1), 16), indexType));
    SemanticOperand selector = s.emit(Op::OP_ADD, indexType, base,
        s.splat(s.integer(operand.m_elementIndex, 16), indexType));
    return s.bitcast(s.emit(Op::OP_SHUFFLE, narrowType, narrow, narrow, selector),
                     s.integerType(e));
}

/////////////////////////////////////////////////////////////////////////////
// Long and wide arithmetic
/////////////////////////////////////////////////////////////////////////////

enum {
    L_ADD, L_SUB, L_ABD, L_MUL, L_QDMUL, L_PMUL
};

// The first source of the long and wide instructions, the second source,
// each extended
void longOperands(SveLift& s, const SveDescriptor& d, uint e, SemanticOperand& a,
                  SemanticOperand& b)
{
    const bool isSigned = (d.m_b & F_UNSIGNED) == 0;
    const bool isTop = (d.m_b & F_TOP) != 0;
    const bool firstTop = (d.m_b & F_TOP_BOTTOM) != 0 ? true :
                          (((d.m_b & F_BOTTOM_TOP) != 0) ? false : isTop);
    const bool secondTop = (d.m_b & F_BOTTOM_TOP) != 0 ? true :
                           (((d.m_b & F_TOP_BOTTOM) != 0) ? false : isTop);
    SemanticOperand n = s.readZ(regOf(s, 1), e);
    if ((d.m_b & F_WIDE) != 0)
        a = n;
    else
        a = narrowElement(s, n, e, firstTop, isSigned);
    b = narrowElement(s, secondSource(s, 2, e), e, secondTop, isSigned);
}

// The 128 bit carry-less products of the quadword form of pmullb / pmullt
SemanticOperand polynomialQuad(SveLift& s, bool isTop)
{
    const SemanticType i64 = SemanticType::integer(64);
    const SemanticType i128 = SemanticType::integer(128);
    const uint segments = s.lanes(128);
    SemanticOperand n = s.readZ(regOf(s, 1), 64);
    SemanticOperand m = s.readZ(regOf(s, 2), 64);
    SemanticOperand result = s.zero(s.integerType(128));
    for (uint segment = 0; segment < segments; segment++)
    {
        SemanticOperand a = s.extractLane(n, 2 * segment + (isTop ? 1 : 0));
        SemanticOperand b = s.extractLane(m, 2 * segment + (isTop ? 1 : 0));
        SemanticOperand low = s.emit(Op::OP_CARRYLESS_MULTIPLY, i64, a, b);
        SemanticOperand high = s.emit(Op::OP_CARRYLESS_MULTIPLY_HIGH, i64, a, b);
        result = s.insertLane(result, s.emit(Op::OP_CONCAT, i128, low, high), segment);
    }
    return result;
}

// saddlb..., ssublb..., sabdlb..., smullb..., sqdmullb..., pmullb... and the
// wide saddwb... m_a: L_*, m_b: F_*
void liftLong(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    if (d.m_a == L_PMUL && e == 128)
    {
        s.writeZ(regOf(s, 0), polynomialQuad(s, (d.m_b & F_TOP) != 0));
        return;
    }
    SemanticOperand a, b;
    longOperands(s, d, e, a, b);
    SemanticOperand result;
    switch (d.m_a)
    {
    case L_ADD: result = s.emit(Op::OP_ADD, type, a, b); break;
    case L_SUB: result = s.emit(Op::OP_SUBTRACT, type, a, b); break;
    case L_ABD:
        result = s.emit(((d.m_b & F_UNSIGNED) != 0) ? Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED
                                                    : Op::OP_ABSOLUTE_DIFFERENCE_SIGNED,
                        type, a, b);
        break;
    case L_MUL: result = s.emit(Op::OP_MULTIPLY, type, a, b); break;
    case L_PMUL: result = s.emit(Op::OP_CARRYLESS_MULTIPLY, type, a, b); break;
    default:
    {
        SemanticOperand product = s.emit(Op::OP_MULTIPLY, type, a, b);
        result = s.emit(Op::OP_ADD_SATURATE_SIGNED, type, product, product);
        break;
    }
    }
    s.writeZ(regOf(s, 0), result);
}

enum {
    A_ABAL, A_MLAL, A_MLSL, A_QDMLAL, A_QDMLSL
};

// sabalb..., smlalb..., smlslb..., sqdmlalb..., sqdmlslb... (also by element)
// m_a: A_*, m_b: F_*
void liftLongAccumulate(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    SemanticOperand a, b;
    longOperands(s, d, e, a, b);
    SemanticOperand accumulator = s.readZ(regOf(s, 0), e);
    SemanticOperand result;
    switch (d.m_a)
    {
    case A_ABAL:
        result = s.emit(Op::OP_ADD, type, accumulator,
            s.emit(((d.m_b & F_UNSIGNED) != 0) ? Op::OP_ABSOLUTE_DIFFERENCE_UNSIGNED
                                               : Op::OP_ABSOLUTE_DIFFERENCE_SIGNED, type, a, b));
        break;
    case A_MLAL:
        result = s.emit(Op::OP_ADD, type, accumulator, s.emit(Op::OP_MULTIPLY, type, a, b));
        break;
    case A_MLSL:
        result = s.emit(Op::OP_SUBTRACT, type, accumulator, s.emit(Op::OP_MULTIPLY, type, a, b));
        break;
    default:
    {
        SemanticOperand product = s.emit(Op::OP_MULTIPLY, type, a, b);
        SemanticOperand doubled = s.emit(Op::OP_ADD_SATURATE_SIGNED, type, product, product);
        result = s.emit(d.m_a == A_QDMLAL ? Op::OP_ADD_SATURATE_SIGNED
                                          : Op::OP_SUBTRACT_SATURATE_SIGNED,
                        type, accumulator, doubled);
        break;
    }
    }
    s.writeZ(regOf(s, 0), result);
}

// sshllb, sshllt, ushllb, ushllt zd, zn, #imm
void liftShiftLong(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    SemanticOperand a = narrowElement(s, s.readZ(regOf(s, 1), e), e,
                                      (d.m_b & F_TOP) != 0, (d.m_b & F_UNSIGNED) == 0);
    s.writeZ(regOf(s, 0), s.emit(Op::OP_SHIFT_LEFT, s.integerType(e), a,
                                 s.splatInteger(operandOf(s, 2).m_immediate, e)));
}

/////////////////////////////////////////////////////////////////////////////
// Narrowing
/////////////////////////////////////////////////////////////////////////////

// Merge the narrow values of a wide lane: 'value' (n bits significant) goes to
// the bottom (zeroing the top) or to the top (keeping the bottom of the
// destination)
void writeNarrow(SveLift& s, uint dest, uint e, const SemanticOperand& value, bool isTop)
{
    const SemanticType type = s.integerType(e);
    const uint n = e / 2;
    const SemanticOperand mask = s.splatInteger((int64)(((uint64)1 << n) - 1), e);
    SemanticOperand low = s.emit(Op::OP_AND, type, value, mask);
    if (!isTop)
    {
        s.writeZ(dest, low);
        return;
    }
    SemanticOperand old = s.emit(Op::OP_AND, type, s.readZ(dest, e), mask);
    s.writeZ(dest, s.emit(Op::OP_OR, type, old, s.emit(Op::OP_SHIFT_LEFT, type, low, s.splatInteger(n, e))));
}

// addhnb, addhnt, raddhnb, raddhnt, subhnb, subhnt, rsubhnb, rsubhnt
// m_a: 0 add, 1 subtract; m_b: 1 top, 2 rounding
void liftHighNarrow(SveLift& s, const SveDescriptor& d)
{
    const uint e = 2 * SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    SemanticOperand a = s.readZ(regOf(s, 1), e);
    SemanticOperand b = s.readZ(regOf(s, 2), e);
    SemanticOperand sum = s.emit(d.m_a == 0 ? Op::OP_ADD : Op::OP_SUBTRACT, type, a, b);
    if ((d.m_b & 2) != 0)
        sum = s.emit(Op::OP_ADD, type, sum, s.splatInteger((int64)((uint64)1 << (e / 2 - 1)), e));
    writeNarrow(s, regOf(s, 0), e,
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, sum, s.splatInteger(e / 2, e)),
                (d.m_b & 1) != 0);
}

enum {
    // The source is signed
    N_SIGNED = 1,
    // The result saturates to the signed range of the narrow element
    N_SAT_SIGNED = 2,
    // The result saturates to the unsigned range
    N_SAT_UNSIGNED = 4,
    // The shift right by an immediate (operand after the source)
    N_SHIFT = 8,
    // The shift rounds
    N_ROUND = 16,
    // The top element
    N_TOP = 32
};

// The narrowing of the e bit lanes of 'x' (optionally shifted right by 'shift')
SemanticOperand narrowValue(SveLift& s, const SemanticOperand& x, uint e, uint flags,
                            int64 shift)
{
    const SemanticType type = s.integerType(e);
    const uint n = e / 2;
    SemanticOperand v = x;
    if ((flags & N_SHIFT) != 0)
    {
        if ((flags & N_ROUND) != 0)
            v = s.emitBehavior(Op::OP_SHIFT_RIGHT_ROUNDING, type, x, s.splatInteger(shift, e),
                               ((flags & N_SIGNED) != 0) ? Op::BEHAVIOR_SIGNED : Op::BEHAVIOR_UNSIGNED);
        else
            v = s.emit(((flags & N_SIGNED) != 0) ? Op::OP_SHIFT_RIGHT_ARITHMETIC
                                                 : Op::OP_SHIFT_RIGHT_LOGICAL,
                       type, x, s.splatInteger(shift, e));
    }
    if ((flags & N_SAT_SIGNED) != 0)
    {
        v = s.emit(Op::OP_MAXIMUM_SIGNED, type,
                   s.emit(Op::OP_MINIMUM_SIGNED, type, v, s.splatInteger((int64)(((uint64)1 << (n - 1)) - 1), e)),
                   s.splatInteger(-(int64)((uint64)1 << (n - 1)), e));
    } else if ((flags & N_SAT_UNSIGNED) != 0)
    {
        const SemanticOperand high = s.splatInteger((int64)(((uint64)1 << n) - 1), e);
        if ((flags & N_SIGNED) != 0)
            v = s.emit(Op::OP_MAXIMUM_SIGNED, type, s.emit(Op::OP_MINIMUM_SIGNED, type, v, high),
                       s.zero(type));
        else
            v = s.emit(Op::OP_MINIMUM_UNSIGNED, type, v, high);
    }
    return v;
}

// sqxtnb... (zd, zn), shrnb... (zd, zn, #imm). m_b: N_*
void liftNarrow(SveLift& s, const SveDescriptor& d)
{
    const uint e = 2 * SveLift::elementBitsOf(operandOf(s, 0));
    SemanticOperand x = s.readZ(regOf(s, 1), e);
    const int64 shift = ((d.m_b & N_SHIFT) != 0) ? operandOf(s, 2).m_immediate : 0;
    writeNarrow(s, regOf(s, 0), e, narrowValue(s, x, e, d.m_b, shift), (d.m_b & N_TOP) != 0);
}

// sqcvtn zd.h, {zn.s, zn+1.s}; sqrshrn zd.h, {zn.s, zn+1.s}, #imm... The two
// vectors are interleaved in the narrow elements of the destination
void liftNarrowPair(SveLift& s, const SveDescriptor& d)
{
    const uint e = 2 * SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const ArmDecodedOperand& list = operandOf(s, 1);
    const uint first = SveLift::registerNumber(list);
    const uint stride = (list.m_stride == 0) ? 1 : list.m_stride;
    const int64 shift = ((d.m_b & N_SHIFT) != 0) ? operandOf(s, 2).m_immediate : 0;
    const uint n = e / 2;
    const SemanticOperand mask = s.splatInteger((int64)(((uint64)1 << n) - 1), e);
    SemanticOperand low = s.emit(Op::OP_AND, type,
        narrowValue(s, s.readZ(first, e), e, d.m_b, shift), mask);
    SemanticOperand high = s.emit(Op::OP_AND, type,
        narrowValue(s, s.readZ((first + stride) & 31, e), e, d.m_b, shift), mask);
    s.writeZ(regOf(s, 0), s.emit(Op::OP_OR, type, low,
                                 s.emit(Op::OP_SHIFT_LEFT, type, high, s.splatInteger(n, e))));
}

/////////////////////////////////////////////////////////////////////////////
// Dot products and matrix multiplies
/////////////////////////////////////////////////////////////////////////////

// sdot, udot, usdot, sudot: zda, zn, zm | zm[i]. m_a: the Behavior of the
// products, m_b: 1 swaps the sources (sudot)
void liftDot(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint sourceBits = SveLift::elementBitsOf(operandOf(s, 1));
    const uint lanes = s.lanes(e) * (e / sourceBits);
    const SemanticType sourceType = SemanticType::vector(SemanticType::integer(sourceBits), lanes);
    SemanticOperand a = s.bitcast(s.readZWhole(regOf(s, 1)), sourceType);
    SemanticOperand b = s.readZWhole(regOf(s, 2));
    const int index = operandOf(s, 2).m_elementIndex;
    if (index >= 0)
    {
        // The group of e bits 'index' of every 128 bit segment
        const SemanticType groupType = s.integerType(e);
        SemanticOperand groups = s.bitcast(b, groupType);
        SemanticOperand lane = s.laneIndexWide(0, 1, s.lanes(e), 16);
        const SemanticType indexType = SemanticType::vector(SemanticType::integer(16), s.lanes(e));
        SemanticOperand base = s.emit(Op::OP_AND, indexType, lane,
            s.splat(s.integer(~(int64)(128 / e - 1), 16), indexType));
        SemanticOperand selector = s.emit(Op::OP_ADD, indexType, base,
            s.splat(s.integer(index, 16), indexType));
        b = s.emit(Op::OP_SHUFFLE, groupType, groups, groups, selector);
    }
    b = s.bitcast(b, sourceType);
    if (d.m_b != 0)
    {
        SemanticOperand t = a;
        a = b;
        b = t;
    }
    SemanticOperand accumulator = s.readZ(regOf(s, 0), e);
    SemanticOperand result = s.context().newTemporary(s.integerType(e));
    SemanticInstruction instruction(Op::OP_DOT_PRODUCT, s.integerType(e), result, a, b, accumulator);
    instruction.setBehavior((Op::Behavior)d.m_a);
    s.context().emit(instruction);
    s.writeZ(regOf(s, 0), result);
}

// smmla, ummla, usmmla zda.s, zn.b, zm.b: per 128 bit segment, a 2x2 matrix of
// 32 bit sums of the products of the rows of 8 bytes. m_a: 0 signed, 1
// unsigned, 2 unsigned by signed
void liftMatrixMultiply(SveLift& s, const SveDescriptor& d)
{
    const SemanticType i32 = SemanticType::integer(32);
    SemanticOperand n = s.readZ(regOf(s, 1), 8);
    SemanticOperand m = s.readZ(regOf(s, 2), 8);
    SemanticOperand accumulator = s.readZ(regOf(s, 0), 32);
    SemanticOperand result = accumulator;
    const bool firstSigned = d.m_a == 0;
    const bool secondSigned = d.m_a != 1;
    for (uint segment = 0; segment < s.vectorBits() / 128; segment++)
    {
        for (uint i = 0; i < 2; i++)
        {
            for (uint j = 0; j < 2; j++)
            {
                SemanticOperand sum = s.extractLane(accumulator, 4 * segment + 2 * i + j);
                for (uint k = 0; k < 8; k++)
                {
                    SemanticOperand x = s.emit(firstSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND, i32,
                                               s.extractLane(n, 16 * segment + 8 * i + k));
                    SemanticOperand y = s.emit(secondSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND, i32,
                                               s.extractLane(m, 16 * segment + 8 * j + k));
                    sum = s.emit(Op::OP_ADD, i32, sum, s.emit(Op::OP_MULTIPLY, i32, x, y));
                }
                result = s.insertLane(result, sum, 4 * segment + 2 * i + j);
            }
        }
    }
    s.writeZ(regOf(s, 0), result);
}

/////////////////////////////////////////////////////////////////////////////
// adr
/////////////////////////////////////////////////////////////////////////////

// adr zd.d, [zn.d, zm.d, sxtw|uxtw {#n}] | zd.t, [zn.t, zm.t, lsl #n]
void liftAddress(SveLift& s, const SveDescriptor& d)
{
    const ArmDecodedOperand& memory = operandOf(s, 1);
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    SemanticOperand base = s.readZ(memory.m_base - ARM64_REG_Z0, e);
    SemanticOperand offset = s.readZ(memory.m_index - ARM64_REG_Z0, e);
    if ((memory.m_shift == ArmDecodedOperand::EXTEND_SXTW) || (memory.m_shift == ArmDecodedOperand::EXTEND_UXTW))
    {
        const SemanticType low = SemanticType::vector(SemanticType::integer(32), s.lanes(e));
        // The low 32 bits of every element, extended
        SemanticOperand shifted = s.emit(Op::OP_SHIFT_LEFT, type, offset, s.splatInteger(32, e));
        offset = s.emit(memory.m_shift == ArmDecodedOperand::EXTEND_SXTW ? Op::OP_SHIFT_RIGHT_ARITHMETIC
                                                                         : Op::OP_SHIFT_RIGHT_LOGICAL,
                        type, shifted, s.splatInteger(32, e));
        (void)low;
    }
    if (memory.m_shiftAmount != 0)
        offset = s.emit(Op::OP_SHIFT_LEFT, type, offset, s.splatInteger(memory.m_shiftAmount, e));
    s.writeZ(regOf(s, 0), s.emit(Op::OP_ADD, type, base, offset));
}

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

#define LONG(name, kind, flags) { name, liftLong, kind, flags }
#define ACC(name, kind, flags) { name, liftLongAccumulate, kind, flags }
#define HN(name, kind, flags) { name, liftHighNarrow, kind, flags }
#define NAR(name, flags) { name, liftNarrow, 0, flags }
#define PAIR(name, flags) { name, liftNarrowPair, 0, flags }

const SveDescriptor gTable[] =
{
    LONG("saddlb", L_ADD, 0), LONG("saddlt", L_ADD, F_TOP),
    LONG("uaddlb", L_ADD, F_UNSIGNED), LONG("uaddlt", L_ADD, F_UNSIGNED | F_TOP),
    LONG("ssublb", L_SUB, 0), LONG("ssublt", L_SUB, F_TOP),
    LONG("usublb", L_SUB, F_UNSIGNED), LONG("usublt", L_SUB, F_UNSIGNED | F_TOP),
    LONG("sabdlb", L_ABD, 0), LONG("sabdlt", L_ABD, F_TOP),
    LONG("uabdlb", L_ABD, F_UNSIGNED), LONG("uabdlt", L_ABD, F_UNSIGNED | F_TOP),
    LONG("smullb", L_MUL, 0), LONG("smullt", L_MUL, F_TOP),
    LONG("umullb", L_MUL, F_UNSIGNED), LONG("umullt", L_MUL, F_UNSIGNED | F_TOP),
    LONG("sqdmullb", L_QDMUL, 0), LONG("sqdmullt", L_QDMUL, F_TOP),
    LONG("pmullb", L_PMUL, F_UNSIGNED), LONG("pmullt", L_PMUL, F_UNSIGNED | F_TOP),
    LONG("saddlbt", L_ADD, F_BOTTOM_TOP), LONG("ssublbt", L_SUB, F_BOTTOM_TOP),
    LONG("ssubltb", L_SUB, F_TOP_BOTTOM),
    LONG("saddwb", L_ADD, F_WIDE), LONG("saddwt", L_ADD, F_WIDE | F_TOP),
    LONG("uaddwb", L_ADD, F_WIDE | F_UNSIGNED), LONG("uaddwt", L_ADD, F_WIDE | F_UNSIGNED | F_TOP),
    LONG("ssubwb", L_SUB, F_WIDE), LONG("ssubwt", L_SUB, F_WIDE | F_TOP),
    LONG("usubwb", L_SUB, F_WIDE | F_UNSIGNED), LONG("usubwt", L_SUB, F_WIDE | F_UNSIGNED | F_TOP),
    ACC("sabalb", A_ABAL, 0), ACC("sabalt", A_ABAL, F_TOP),
    ACC("uabalb", A_ABAL, F_UNSIGNED), ACC("uabalt", A_ABAL, F_UNSIGNED | F_TOP),
    ACC("smlalb", A_MLAL, 0), ACC("smlalt", A_MLAL, F_TOP),
    ACC("umlalb", A_MLAL, F_UNSIGNED), ACC("umlalt", A_MLAL, F_UNSIGNED | F_TOP),
    ACC("smlslb", A_MLSL, 0), ACC("smlslt", A_MLSL, F_TOP),
    ACC("umlslb", A_MLSL, F_UNSIGNED), ACC("umlslt", A_MLSL, F_UNSIGNED | F_TOP),
    ACC("sqdmlalb", A_QDMLAL, 0), ACC("sqdmlalt", A_QDMLAL, F_TOP),
    ACC("sqdmlslb", A_QDMLSL, 0), ACC("sqdmlslt", A_QDMLSL, F_TOP),
    ACC("sqdmlalbt", A_QDMLAL, F_BOTTOM_TOP), ACC("sqdmlslbt", A_QDMLSL, F_BOTTOM_TOP),
    HN("addhnb", 0, 0), HN("addhnt", 0, 1), HN("raddhnb", 0, 2), HN("raddhnt", 0, 3),
    HN("subhnb", 1, 0), HN("subhnt", 1, 1), HN("rsubhnb", 1, 2), HN("rsubhnt", 1, 3),
    NAR("sqxtnb", N_SIGNED | N_SAT_SIGNED), NAR("sqxtnt", N_SIGNED | N_SAT_SIGNED | N_TOP),
    NAR("uqxtnb", N_SAT_UNSIGNED), NAR("uqxtnt", N_SAT_UNSIGNED | N_TOP),
    NAR("sqxtunb", N_SIGNED | N_SAT_UNSIGNED), NAR("sqxtunt", N_SIGNED | N_SAT_UNSIGNED | N_TOP),
    NAR("shrnb", N_SHIFT), NAR("shrnt", N_SHIFT | N_TOP),
    NAR("rshrnb", N_SHIFT | N_ROUND), NAR("rshrnt", N_SHIFT | N_ROUND | N_TOP),
    NAR("sqshrnb", N_SIGNED | N_SAT_SIGNED | N_SHIFT),
    NAR("sqshrnt", N_SIGNED | N_SAT_SIGNED | N_SHIFT | N_TOP),
    NAR("sqrshrnb", N_SIGNED | N_SAT_SIGNED | N_SHIFT | N_ROUND),
    NAR("sqrshrnt", N_SIGNED | N_SAT_SIGNED | N_SHIFT | N_ROUND | N_TOP),
    NAR("uqshrnb", N_SAT_UNSIGNED | N_SHIFT), NAR("uqshrnt", N_SAT_UNSIGNED | N_SHIFT | N_TOP),
    NAR("uqrshrnb", N_SAT_UNSIGNED | N_SHIFT | N_ROUND),
    NAR("uqrshrnt", N_SAT_UNSIGNED | N_SHIFT | N_ROUND | N_TOP),
    NAR("sqshrunb", N_SIGNED | N_SAT_UNSIGNED | N_SHIFT),
    NAR("sqshrunt", N_SIGNED | N_SAT_UNSIGNED | N_SHIFT | N_TOP),
    NAR("sqrshrunb", N_SIGNED | N_SAT_UNSIGNED | N_SHIFT | N_ROUND),
    NAR("sqrshrunt", N_SIGNED | N_SAT_UNSIGNED | N_SHIFT | N_ROUND | N_TOP),
    PAIR("sqcvtn", N_SIGNED | N_SAT_SIGNED), PAIR("uqcvtn", N_SAT_UNSIGNED),
    PAIR("sqcvtun", N_SIGNED | N_SAT_UNSIGNED),
    PAIR("sqrshrn", N_SIGNED | N_SAT_SIGNED | N_SHIFT | N_ROUND),
    PAIR("uqrshrn", N_SAT_UNSIGNED | N_SHIFT | N_ROUND),
    PAIR("sqrshrun", N_SIGNED | N_SAT_UNSIGNED | N_SHIFT | N_ROUND),
    { "sshllb", liftShiftLong, 0, 0 }, { "sshllt", liftShiftLong, 0, F_TOP },
    { "ushllb", liftShiftLong, 0, F_UNSIGNED }, { "ushllt", liftShiftLong, 0, F_UNSIGNED | F_TOP },
    { "sdot", liftDot, Op::BEHAVIOR_SIGNED, 0 },
    { "udot", liftDot, Op::BEHAVIOR_UNSIGNED, 0 },
    { "usdot", liftDot, Op::BEHAVIOR_UNSIGNED_SIGNED, 0 },
    { "sudot", liftDot, Op::BEHAVIOR_UNSIGNED_SIGNED, 1 },
    { "smmla", liftMatrixMultiply, 0, 0 },
    { "ummla", liftMatrixMultiply, 1, 0 },
    { "usmmla", liftMatrixMultiply, 2, 0 },
    { "adr", liftAddress, 0, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSve2Wide(ArmLiftContext& context)
{
    runSveTable(context, gTable);
}
