/*
 * Arm64SemanticLiftSveFloat.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the floating point instructions: the arithmetic,
 * the fused multiply add family (with the indexed forms), the complex
 * number instructions, the compares, the reductions (fadda in strict order),
 * the conversions with the integers and between the formats, the rounding
 * and the helpers of the trigonometric functions. The floating point
 * environment is fpcr (rounding, flush to zero, default NaN) and fpsr; the
 * lanes of a predicated instruction which are not active raise no exception
 * (their operands are replaced by a neutral value before the operation).
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

// The bits of a floating point number of 'bits' bits (16, 32, 64) for the
// values of the immediates and the neutral elements: 0, 1, 2, 0.5... (normal
// numbers or zero, exact in every format)
uint64 floatBits(double value, uint bits)
{
    union { double m_double; uint64 m_bits; } converter;
    converter.m_double = value;
    const uint64 sign = converter.m_bits >> 63;
    const uint64 exponent = (converter.m_bits >> 52) & 0x7FF;
    const uint64 fraction = converter.m_bits & 0xFFFFFFFFFFFFFULL;
    uint exponentBits = 11, fractionBits = 52, bias = 1023;
    if (bits == 16)
    {
        exponentBits = 5;
        fractionBits = 10;
        bias = 15;
    } else if (bits == 32)
    {
        exponentBits = 8;
        fractionBits = 23;
        bias = 127;
    }
    if (exponent == 0)
        return sign << (bits - 1);
    const uint64 target = exponent - 1023 + bias;
    (void)exponentBits;
    return (sign << (bits - 1)) | (target << fractionBits) |
           (fraction >> (52 - fractionBits));
}

SemanticOperand floatVector(SveLift& s, const SemanticOperand& integerVector, uint e)
{
    return s.bitcast(integerVector, s.floatType(e));
}

SemanticOperand integerVectorOf(SveLift& s, const SemanticOperand& floatVector, uint e)
{
    return s.bitcast(floatVector, s.integerType(e));
}

SemanticOperand readFloat(SveLift& s, uint operandIndex, uint e)
{
    return floatVector(s, s.readZOperand(operandIndex, e), e);
}

// A floating point constant in every lane
SemanticOperand floatSplat(SveLift& s, double value, uint e)
{
    return floatVector(s, s.splatInteger((int64)floatBits(value, e), e), e);
}

// The inactive lanes of a floating point vector replaced by 'neutral', so
// that the operation raises no exception for them
SemanticOperand activeOnly(SveLift& s, const SemanticOperand& mask,
                           const SemanticOperand& value, double neutral, uint e)
{
    return s.select(mask, value, floatSplat(s, neutral, e));
}

// The default NaN rule of fpcr.DN on the result
SemanticOperand finishNan(SveLift& s, const SemanticOperand& result)
{
    return s.applyDefaultNan(result, result.getType());
}

// Write the floating point result of the instruction (destination operand 0)
// governed by the predicate operand 'predicateIndex' (-1: unpredicated)
void writeFloat(SveLift& s, int predicateIndex, uint e, const SemanticOperand& result)
{
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand value = integerVectorOf(s, result, e);
    if (predicateIndex >= 0)
    {
        SemanticOperand mask = s.readMaskOperand(predicateIndex, e);
        const bool isZeroing = operandOf(s, predicateIndex).m_predicateQualifier ==
                               ArmDecodedOperand::PREDICATE_ZEROING;
        value = s.predicate(mask, value, s.readZ(reg, e), isZeroing);
    }
    s.writeZ(reg, value);
}

/////////////////////////////////////////////////////////////////////////////
// Binary arithmetic
/////////////////////////////////////////////////////////////////////////////

enum {
    B_ADD, B_SUB, B_SUBR, B_MUL, B_DIV, B_DIVR, B_MAX, B_MIN, B_MAXNM, B_MINNM,
    B_ABD, B_SCALE, B_MULX, B_TSMUL, B_RECPS
};

// fadd, fsub, fsubr, fmul, fdiv, fdivr, fmax, fmin, fmaxnm, fminnm, fabd,
// fscale, fmulx (predicated, with an immediate for some) and the unpredicated
// fadd, fsub, fmul, ftsmul, frecps (frsqrts is in Arm64SemanticLiftSveFloat2.cpp). m_a: B_*
void liftMultiplyIndexed(SveLift& s, const SveDescriptor& d);

void liftBinary(SveLift& s, const SveDescriptor& d)
{
    if ((d.m_a == B_MUL) && (s.context().getOperandsCount() == 3) &&
        (operandOf(s, 2).m_elementIndex >= 0))
    {
        liftMultiplyIndexed(s, d);
        return;
    }
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    int predicateIndex = -1;
    uint first = 1;
    if (isPredicateAt(s, 1))
    {
        predicateIndex = 1;
        first = 2;
    }
    SemanticOperand mask;
    if (predicateIndex >= 0)
        mask = s.readMaskOperand(predicateIndex, e);
    SemanticOperand a = readFloat(s, first, e);
    SemanticOperand b;
    if (operandOf(s, first + 1).m_kind == ArmDecodedOperand::KIND_FLOAT_IMMEDIATE)
        b = floatSplat(s, operandOf(s, first + 1).m_floatImmediate, e);
    else if (d.m_a == B_SCALE)
    {
        // The exponent: a signed integer, clamped, converted
        const int64 limit = (e == 16) ? 64 : ((e == 32) ? 300 : 2200);
        const SemanticType integer = s.integerType(e);
        SemanticOperand n = s.readZOperand(first + 1, e);
        SemanticOperand high = s.splatInteger(limit, e);
        SemanticOperand low = s.splatInteger(-limit, e);
        n = s.select(s.compareLanes(Op::COND_SIGNED_GREATER, integer, n, high), high, n);
        n = s.select(s.compareLanes(Op::COND_SIGNED_LESS, integer, n, low), low, n);
        b = s.emit(Op::OP_SIGNED_TO_FLOAT, type, n);
    } else
        b = readFloat(s, first + 1, e);
    const SemanticOperand old = a;
    // The operation, with neutral values in the inactive lanes
    double neutralA = 0.0, neutralB = 0.0;
    if (d.m_a == B_DIV || d.m_a == B_DIVR)
        neutralB = 1.0;
    if (d.m_a == B_DIVR)
    {
        neutralA = 1.0;
        neutralB = 0.0;
    }
    if (mask.isValid())
    {
        a = activeOnly(s, mask, a, neutralA, e);
        if (d.m_a == B_SCALE)
            b = s.select(mask, b, floatSplat(s, 0.0, e));
        else
            b = activeOnly(s, mask, b, neutralB, e);
    }
    SemanticOperand result;
    switch (d.m_a)
    {
    case B_ADD: result = s.emitFloat(Op::OP_FLOAT_ADD, type, a, b); break;
    case B_SUB: result = s.emitFloat(Op::OP_FLOAT_SUBTRACT, type, a, b); break;
    case B_SUBR: result = s.emitFloat(Op::OP_FLOAT_SUBTRACT, type, b, a); break;
    case B_MUL: result = s.emitFloat(Op::OP_FLOAT_MULTIPLY, type, a, b); break;
    case B_DIV: result = s.emitFloat(Op::OP_FLOAT_DIVIDE, type, a, b); break;
    case B_DIVR: result = s.emitFloat(Op::OP_FLOAT_DIVIDE, type, b, a); break;
    case B_MAX: result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, a, b, SemanticOperand(),
                                     Op::BEHAVIOR_MINMAX_PROPAGATE_NAN); break;
    case B_MIN: result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, a, b, SemanticOperand(),
                                     Op::BEHAVIOR_MINMAX_PROPAGATE_NAN); break;
    case B_MAXNM: result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, a, b, SemanticOperand(),
                                       Op::BEHAVIOR_MINMAX_NUMBER); break;
    case B_MINNM: result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, a, b, SemanticOperand(),
                                       Op::BEHAVIOR_MINMAX_NUMBER); break;
    case B_ABD:
        result = s.emitFloat(Op::OP_FLOAT_SUBTRACT, type, a, b);
        result = finishNan(s, result);
        result = s.emit(Op::OP_FLOAT_ABSOLUTE, type, result);
        writeFloat(s, predicateIndex, e, result);
        return;
    case B_SCALE: result = s.emitFloat(Op::OP_FLOAT_SCALE, type, a, b); break;
    case B_MULX:
        // 0 * infinity is 2.0 with the sign of the product
        result = s.emitFloat(Op::OP_FLOAT_MULTIPLY, type, a, b, SemanticOperand(),
                             Op::BEHAVIOR_MULTIPLY_EXTENDED);
        break;
    case B_TSMUL:
    {
        // ftsmul: a * a, the sign bit cleared and set from bit 0 of b
        SemanticOperand square = finishNan(s, s.emitFloat(Op::OP_FLOAT_MULTIPLY, type, a, a));
        SemanticOperand bits = integerVectorOf(s, square, e);
        SemanticOperand cleared = s.emit(Op::OP_AND, s.integerType(e), bits,
            s.splatInteger((int64)(e == 64 ? 0x7FFFFFFFFFFFFFFFULL : (((uint64)1 << (e - 1)) - 1)), e));
        SemanticOperand bit0 = s.emit(Op::OP_AND, s.integerType(e), s.readZOperand(first + 1, e),
                                      s.splatInteger(1, e));
        SemanticOperand sign = s.emit(Op::OP_SHIFT_LEFT, s.integerType(e), bit0, s.splatInteger(e - 1, e));
        SemanticOperand isNan = s.compareLanes(Op::COND_FLOAT_UNORDERED, type, square, square);
        SemanticOperand placed = s.emit(Op::OP_OR, s.integerType(e), cleared, sign);
        writeFloat(s, predicateIndex, e, floatVector(s, s.select(isNan, bits, placed), e));
        return;
    }
    case B_RECPS:
        // 2 - a * b, fused; 2.0 for infinity * 0
        result = s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, type, a, b, SemanticOperand(),
                             Op::BEHAVIOR_STEP_RECIPROCAL);
        break;
    }
    (void)old;
    writeFloat(s, predicateIndex, e, finishNan(s, result));
}

/////////////////////////////////////////////////////////////////////////////
// Unary operations
/////////////////////////////////////////////////////////////////////////////

enum {
    U_ABS, U_NEG, U_SQRT, U_RINTA, U_RINTI, U_RINTM, U_RINTN, U_RINTP, U_RINTX, U_RINTZ
};

// fabs, fneg, fsqrt, frint*. m_a: U_*
void liftUnary(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    int predicateIndex = -1;
    uint source = 1;
    if (isPredicateAt(s, 1))
    {
        predicateIndex = 1;
        source = 2;
    }
    SemanticOperand a = readFloat(s, source, e);
    SemanticOperand result;
    if (d.m_a == U_ABS)
    {
        result = s.emit(Op::OP_FLOAT_ABSOLUTE, type, a);
        writeFloat(s, predicateIndex, e, result);
        return;
    }
    if (d.m_a == U_NEG)
    {
        result = s.emit(Op::OP_FLOAT_NEGATE, type, a);
        writeFloat(s, predicateIndex, e, result);
        return;
    }
    if (predicateIndex >= 0)
        a = activeOnly(s, s.readMaskOperand(predicateIndex, e), a, 0.0, e);
    if (d.m_a == U_SQRT)
    {
        result = s.emitFloat(Op::OP_FLOAT_SQUARE_ROOT, type, a);
    } else
    {
        Op::RoundingMode mode = Op::ROUND_DYNAMIC;
        uint kind = Op::FLOAT_ROUND_SUPPRESS_INEXACT;
        switch (d.m_a)
        {
        case U_RINTA: mode = Op::ROUND_NEAREST_AWAY; break;
        case U_RINTI: break;
        case U_RINTM: mode = Op::ROUND_TOWARD_NEGATIVE; break;
        case U_RINTN: mode = Op::ROUND_NEAREST_EVEN; break;
        case U_RINTP: mode = Op::ROUND_TOWARD_POSITIVE; break;
        case U_RINTX: kind = Op::FLOAT_ROUND_SIGNAL_INEXACT; break;
        default: mode = Op::ROUND_TOWARD_ZERO; break;
        }
        SemanticOperand t = s.context().newTemporary(type);
        SemanticInstruction instruction(Op::OP_FLOAT_ROUND, type, t, a);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
        instruction.setRoundingMode(mode);
        instruction.setKind(kind);
        s.context().emit(instruction);
        result = t;
    }
    writeFloat(s, predicateIndex, e, finishNan(s, result));
}

/////////////////////////////////////////////////////////////////////////////
// Fused multiply add
/////////////////////////////////////////////////////////////////////////////

// The vector of the elements 'index' of every 128 bit segment of the operand
SemanticOperand segmentElement(SveLift& s, const SemanticOperand& vector, uint e,
                               uint index)
{
    const uint segmentLanes = 128 / e;
    if (s.lanes(e) <= segmentLanes)
        return s.emit(Op::OP_SHUFFLE, vector.getType(), vector, vector,
                      s.splatInteger(index, e));
    SemanticOperand lanes = s.laneIndex(0, 1, e);
    SemanticOperand base = s.emit(Op::OP_AND, s.integerType(e), lanes,
                                  s.splatInteger(~(int64)(segmentLanes - 1), e));
    SemanticOperand selector = s.emit(Op::OP_ADD, s.integerType(e), base, s.splatInteger(index, e));
    return s.emit(Op::OP_SHUFFLE, vector.getType(), vector, vector, selector);
}

// fmla, fmls, fnmla, fnmls (zda, pg/m, zn, zm | indexed zda, zn, zm[i]) and
// fmad, fmsb, fnmad, fnmsb. m_a: 0 fmla, 1 fmls, 2 fnmla, 3 fnmls, 4 fmad,
// 5 fmsb, 6 fnmad, 7 fnmsb
void liftFusedMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    const bool isPredicated = isPredicateAt(s, 1);
    const uint first = isPredicated ? 2 : 1;
    SemanticOperand n, m, addend;
    if (d.m_a < 4)
    {
        n = readFloat(s, first, e);
        if (operandOf(s, first + 1).m_elementIndex >= 0)
            m = floatVector(s, segmentElement(s, s.readZOperand(first + 1, e), e,
                                              operandOf(s, first + 1).m_elementIndex), e);
        else
            m = readFloat(s, first + 1, e);
        addend = floatVector(s, s.readZ(reg, e), e);
    } else
    {
        // zdn, pg/m, zm, za: za + zdn * zm
        n = floatVector(s, s.readZ(reg, e), e);
        m = readFloat(s, first, e);
        addend = readFloat(s, first + 1, e);
    }
    const bool negateProduct = (d.m_a & 3) == 1 || (d.m_a & 3) == 2;
    const bool negateAddend = (d.m_a & 3) >= 2;
    if (isPredicated)
    {
        SemanticOperand mask = s.readMaskOperand(1, e);
        n = activeOnly(s, mask, n, 0.0, e);
        m = activeOnly(s, mask, m, 0.0, e);
        addend = activeOnly(s, mask, addend, 0.0, e);
    }
    if (negateProduct)
        n = s.emit(Op::OP_FLOAT_NEGATE, type, n);
    if (negateAddend)
        addend = s.emit(Op::OP_FLOAT_NEGATE, type, addend);
    SemanticOperand result = s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, type, n, m, addend);
    writeFloat(s, isPredicated ? 1 : -1, e, finishNan(s, result));
}

// fmul (indexed) zd, zn, zm[i]
void liftMultiplyIndexed(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    SemanticOperand n = readFloat(s, 1, e);
    SemanticOperand m = floatVector(s, segmentElement(s, s.readZOperand(2, e), e,
                                    operandOf(s, 2).m_elementIndex), e);
    SemanticOperand result = s.emitFloat(Op::OP_FLOAT_MULTIPLY, type, n, m);
    writeFloat(s, -1, e, finishNan(s, result));
}

/////////////////////////////////////////////////////////////////////////////
// Complex numbers
/////////////////////////////////////////////////////////////////////////////

// fcadd zdn, pg/m, zdn, zm, #90|#270
void liftComplexAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    const SemanticType scalar = type.getElementType();
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand a = readFloat(s, 2, e);
    SemanticOperand b = readFloat(s, 3, e);
    const bool isRotate270 = operandOf(s, 4).m_immediate == 270;
    const uint count = s.lanes(e);
    SemanticOperand maskBits = s.bitcast(mask, SemanticType::integer(count));
    SemanticOperand zeroScalar = s.context().getImmediate(0, scalar);
    SemanticOperand result = a;
    for (uint pair = 0; pair < count / 2; pair++)
    {
        // The operands of an inactive destination element are zeros (the
        // inactive source elements still feed the active ones)
        SemanticOperand activeRe = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
            s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count), maskBits, s.integer(2 * pair, count)));
        SemanticOperand activeIm = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
            s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count), maskBits, s.integer(2 * pair + 1, count)));
        SemanticOperand aRe = s.select(activeRe, s.extractLane(a, 2 * pair), zeroScalar);
        SemanticOperand aIm = s.select(activeIm, s.extractLane(a, 2 * pair + 1), zeroScalar);
        SemanticOperand bRe = s.select(activeIm, s.extractLane(b, 2 * pair), zeroScalar);
        SemanticOperand bIm = s.select(activeRe, s.extractLane(b, 2 * pair + 1), zeroScalar);
        // The rotation negates an operand of the addition
        if (!isRotate270)
            bIm = s.emit(Op::OP_FLOAT_NEGATE, scalar, bIm);
        else
            bRe = s.emit(Op::OP_FLOAT_NEGATE, scalar, bRe);
        SemanticOperand re = s.emitFloat(Op::OP_FLOAT_ADD, scalar, aRe, bIm);
        SemanticOperand im = s.emitFloat(Op::OP_FLOAT_ADD, scalar, aIm, bRe);
        result = s.insertLane(result, finishNan(s, re), 2 * pair);
        result = s.insertLane(result, finishNan(s, im), 2 * pair + 1);
    }
    writeFloat(s, 1, e, result);
}

// fcmla zda, pg/m, zn, zm, #rot | zda, zn, zm[i], #rot
void liftComplexMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    const SemanticType scalar = type.getElementType();
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    const bool isPredicated = isPredicateAt(s, 1);
    const uint first = isPredicated ? 2 : 1;
    SemanticOperand n = readFloat(s, first, e);
    SemanticOperand m;
    const bool isIndexed = operandOf(s, first + 1).m_elementIndex >= 0;
    if (isIndexed)
    {
        // The complex element of every 128 bit segment
        const uint pairsPerSegment = 128 / (2 * e);
        SemanticOperand pairs = s.readZOperand(first + 1, 2 * e);
        SemanticOperand lanes = s.laneIndex(0, 1, 2 * e);
        SemanticOperand base = s.emit(Op::OP_AND, s.integerType(2 * e), lanes,
                                      s.splatInteger(~(int64)(pairsPerSegment - 1), 2 * e));
        SemanticOperand selector = s.emit(Op::OP_ADD, s.integerType(2 * e), base,
                                          s.splatInteger(operandOf(s, first + 1).m_elementIndex, 2 * e));
        m = floatVector(s, s.bitcast(s.emit(Op::OP_SHUFFLE, s.integerType(2 * e), pairs, pairs, selector),
                                     s.integerType(e)), e);
    } else
    {
        m = readFloat(s, first + 1, e);
    }
    SemanticOperand accumulator = floatVector(s, s.readZ(reg, e), e);
    const uint rotation = (uint)(operandOf(s, first + 2).m_immediate / 90) & 3;
    SemanticOperand mask;
    if (isPredicated)
        mask = s.readMaskOperand(1, e);
    SemanticOperand result = accumulator;
    for (uint pair = 0; pair < s.lanes(e) / 2; pair++)
    {
        const uint re = 2 * pair, im = 2 * pair + 1;
        SemanticOperand n0 = s.extractLane(n, re), n1 = s.extractLane(n, im);
        SemanticOperand m0 = s.extractLane(m, re), m1 = s.extractLane(m, im);
        SemanticOperand e1, e2, e3, e4;
        switch (rotation)
        {
        case 0: e1 = n0; e2 = m0; e3 = n0; e4 = m1; break;
        case 1: e1 = n1; e2 = s.emit(Op::OP_FLOAT_NEGATE, scalar, m1); e3 = n1; e4 = m0; break;
        case 2: e1 = n0; e2 = s.emit(Op::OP_FLOAT_NEGATE, scalar, m0); e3 = n0;
                e4 = s.emit(Op::OP_FLOAT_NEGATE, scalar, m1); break;
        default: e1 = n1; e2 = m1; e3 = n1; e4 = s.emit(Op::OP_FLOAT_NEGATE, scalar, m0); break;
        }
        SemanticOperand acc0 = s.extractLane(accumulator, re), acc1 = s.extractLane(accumulator, im);
        if (isPredicated)
        {
            // Inactive elements: multiply zeros so that nothing is raised
            const SemanticType bitsType = SemanticType::integer(s.lanes(e));
            SemanticOperand bits = s.bitcast(mask, bitsType);
            SemanticOperand a0 = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, bitsType, bits, s.integer(re, s.lanes(e))));
            SemanticOperand a1 = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, bitsType, bits, s.integer(im, s.lanes(e))));
            SemanticOperand zeroScalar = s.context().getImmediate(0, scalar);
            e1 = s.select(a0, e1, zeroScalar);
            e2 = s.select(a0, e2, zeroScalar);
            e3 = s.select(a1, e3, zeroScalar);
            e4 = s.select(a1, e4, zeroScalar);
            acc0 = s.select(a0, acc0, zeroScalar);
            acc1 = s.select(a1, acc1, zeroScalar);
        }
        SemanticOperand r0 = finishNan(s, s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, scalar, e1, e2, acc0));
        SemanticOperand r1 = finishNan(s, s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, scalar, e3, e4, acc1));
        result = s.insertLane(result, r0, re);
        result = s.insertLane(result, r1, im);
    }
    writeFloat(s, isPredicated ? 1 : -1, e, result);
}

/////////////////////////////////////////////////////////////////////////////
// Compares
/////////////////////////////////////////////////////////////////////////////

// fcmeq, fcmge, fcmgt, fcmne, fcmuo, fcmle, fcmlt, facge, facgt:
// pd, pg/z, zn, zm | #0.0. m_a: the condition, m_b: 1 absolute values
void liftCompare(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    SemanticOperand governing = s.readMaskOperand(1, e);
    SemanticOperand a = readFloat(s, 2, e);
    SemanticOperand b;
    if (operandOf(s, 3).m_kind == ArmDecodedOperand::KIND_FLOAT_IMMEDIATE)
        b = floatSplat(s, 0.0, e);
    else
        b = readFloat(s, 3, e);
    a = activeOnly(s, governing, a, 0.0, e);
    b = activeOnly(s, governing, b, 0.0, e);
    if (d.m_b != 0)
    {
        a = s.emit(Op::OP_FLOAT_ABSOLUTE, type, a);
        b = s.emit(Op::OP_FLOAT_ABSOLUTE, type, b);
    }
    SemanticOperand result = s.compareLanes((Op::Condition)d.m_a, type, a, b);
    result = s.emit(Op::OP_AND, SemanticType::mask(s.lanes(e)), result, governing);
    s.writeLaneMask(SveLift::registerNumber(operandOf(s, 0)), result, e);
}

/////////////////////////////////////////////////////////////////////////////
// Reductions
/////////////////////////////////////////////////////////////////////////////

// faddv, fmaxv, fminv, fmaxnmv, fminnmv: a SIMD scalar register. m_a: the
// combine operation, m_b: the behavior; the identity of the inactive lanes
void liftReduce(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 2));
    const SemanticType type = s.floatType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand a = readFloat(s, 2, e);
    // The identity: +0 for the sum, -infinity / +infinity for fmax / fmin, a
    // quiet NaN for fmaxnm / fminnm
    uint64 identity;
    const uint64 infinity = (e == 16) ? 0x7C00ULL : ((e == 32) ? 0x7F800000ULL : 0x7FF0000000000000ULL);
    const uint64 sign = (uint64)1 << (e - 1);
    const uint64 quietNan = (e == 16) ? 0x7E00ULL : ((e == 32) ? 0x7FC00000ULL : 0x7FF8000000000000ULL);
    if (d.m_a == Op::OP_FLOAT_ADD)
        identity = 0;
    else if (d.m_b == Op::BEHAVIOR_MINMAX_NUMBER)
        identity = quietNan;
    else if (d.m_a == Op::OP_FLOAT_MAXIMUM)
        identity = infinity | sign;
    else
        identity = infinity;
    a = s.select(mask, a, floatVector(s, s.splatInteger((int64)identity, e), e));
    SemanticOperand scalar = s.context().newTemporary(type.getElementType());
    SemanticInstruction reduce(Op::OP_REDUCE, type, scalar, a);
    reduce.setCombineOperation((Op::Operation)d.m_a);
    reduce.setBehavior(d.m_a == Op::OP_FLOAT_ADD ? Op::BEHAVIOR_REDUCE_PAIRWISE
                                                  : (Op::Behavior)d.m_b);
    reduce.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    s.context().emit(reduce);
    // A NaN result is the default NaN under fpcr.DN, like the other operations
    SemanticOperand result = finishNan(s, scalar);
    s.context().move(s.context().getRegister(operandOf(s, 0).m_register),
                     s.bitcast(result, SemanticType::integer(e)));
}

// fadda sd, pg, sd, zm: the sum in order of the active elements
void liftOrderedAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 3));
    const SemanticType type = s.floatType(e);
    const SemanticType scalar = type.getElementType();
    const uint count = s.lanes(e);
    SemanticOperand maskBits = s.bitcast(s.readMaskOperand(1, e), SemanticType::integer(count));
    SemanticOperand a = readFloat(s, 3, e);
    SemanticOperand initial = s.bitcast(s.truncate(s.context().getRegister(operandOf(s, 0).m_register), e), scalar);
    SemanticOperand accumulator = initial;
    SemanticOperand zeroScalar = s.context().getImmediate(0, scalar);
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
            s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count), maskBits, s.integer(lane, count)));
        // An inactive element adds zeros to zero: no exception, the sum is dropped
        SemanticOperand sum = finishNan(s, s.emitFloat(Op::OP_FLOAT_ADD, scalar,
            s.select(active, accumulator, zeroScalar),
            s.select(active, s.extractLane(a, lane), zeroScalar)));
        accumulator = s.select(active, sum, accumulator);
    }
    s.context().move(s.context().getRegister(operandOf(s, 0).m_register),
                     s.bitcast(accumulator, SemanticType::integer(e)));
}

/////////////////////////////////////////////////////////////////////////////
// Conversions
/////////////////////////////////////////////////////////////////////////////

SemanticType::FloatFormat formatOf(uint bits)
{
    return (bits == 16) ? SemanticType::FLOAT_HALF :
           ((bits == 32) ? SemanticType::FLOAT_SINGLE : SemanticType::FLOAT_DOUBLE);
}

// fcvt, fcvtzs, fcvtzu, scvtf, ucvtf, fcvtx: the elements of the source and
// the destination live in containers of the larger size (the predicate is
// the container size); lane by lane
// m_a: 0 fcvt, 1 fcvtzs, 2 fcvtzu, 3 scvtf, 4 ucvtf, 5 fcvtx
void liftConvert(SveLift& s, const SveDescriptor& d)
{
    if (!isPredicateAt(s, 1))
    {
        // The unpredicated forms of SVE2.2 / SVE2.3 are not lifted
        s.undefined();
        return;
    }
    const uint destinationBits = SveLift::elementBitsOf(operandOf(s, 0));
    const uint sourceBits = SveLift::elementBitsOf(operandOf(s, 2));
    const uint container = (destinationBits > sourceBits) ? destinationBits : sourceBits;
    const uint count = s.lanes(container);
    SemanticOperand mask = s.readMaskOperand(1, container);
    SemanticOperand maskBits = s.bitcast(mask, SemanticType::integer(count));
    SemanticOperand source = s.readZOperand(2, container);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand old = s.readZ(reg, container);
    const bool isZeroing = operandOf(s, 1).m_predicateQualifier == ArmDecodedOperand::PREDICATE_ZEROING;
    SemanticOperand result = isZeroing ? s.zero(s.integerType(container)) : old;
    const SemanticType destinationFloat = SemanticType::floating(formatOf(destinationBits));
    const SemanticType sourceFloat = SemanticType::floating(formatOf(sourceBits));
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
            s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count), maskBits, s.integer(lane, count)));
        SemanticOperand wide = s.extractLane(source, lane);
        SemanticOperand narrow = (sourceBits < container) ? s.truncate(wide, sourceBits) : wide;
        SemanticOperand value;
        SemanticOperand t;
        switch (d.m_a)
        {
        case 0:
        case 5:
        {
            // float to float; inactive lanes convert zero
            SemanticOperand x = s.select(active, s.bitcast(narrow, sourceFloat), s.context().getImmediate(0, sourceFloat));
            SemanticOperand r = s.context().newTemporary(destinationFloat);
            SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, destinationFloat, r, x);
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
            if (d.m_a == 5)
                instruction.setRoundingMode(Op::ROUND_TO_ODD);
            s.context().emit(instruction);
            value = s.bitcast(finishNan(s, r), SemanticType::integer(destinationBits));
            break;
        }
        case 1:
        case 2:
        {
            SemanticOperand x = s.select(active, s.bitcast(narrow, sourceFloat), s.context().getImmediate(0, sourceFloat));
            SemanticOperand r = s.context().newTemporary(SemanticType::integer(destinationBits));
            SemanticInstruction instruction(d.m_a == 1 ? Op::OP_FLOAT_TO_SIGNED : Op::OP_FLOAT_TO_UNSIGNED,
                SemanticType::integer(destinationBits), r, x);
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
            instruction.setRoundingMode(Op::ROUND_TOWARD_ZERO);
            instruction.setBehavior(Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO);
            s.context().emit(instruction);
            value = r;
            break;
        }
        default:
        {
            SemanticOperand x = s.select(active, narrow, s.integer(0, sourceBits));
            SemanticOperand r = s.context().newTemporary(destinationFloat);
            SemanticInstruction instruction(d.m_a == 3 ? Op::OP_SIGNED_TO_FLOAT : Op::OP_UNSIGNED_TO_FLOAT,
                destinationFloat, r, x);
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
            s.context().emit(instruction);
            value = s.bitcast(r, SemanticType::integer(destinationBits));
            break;
        }
        }
        (void)t;
        // The destination element is written as a whole container (the
        // converted value zero extended, or sign extended for the integers)
        SemanticOperand wideValue = value;
        if (destinationBits < container)
            wideValue = s.emit(d.m_a == 1 ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                               SemanticType::integer(container), value);
        SemanticOperand current = s.extractLane(result, lane);
        result = s.insertLane(result, s.select(active, wideValue, current), lane);
    }
    s.writeZ(reg, result);
}

/////////////////////////////////////////////////////////////////////////////
// The tables
/////////////////////////////////////////////////////////////////////////////

#define FBIN(name, id) { name, liftBinary, id, 0 }
#define FUN(name, id) { name, liftUnary, id, 0 }

const SveDescriptor gFloatTable[] =
{
    FBIN("fadd", B_ADD), FBIN("fsub", B_SUB), FBIN("fsubr", B_SUBR), FBIN("fmul", B_MUL),
    FBIN("fdiv", B_DIV), FBIN("fdivr", B_DIVR), FBIN("fmax", B_MAX), FBIN("fmin", B_MIN),
    FBIN("fmaxnm", B_MAXNM), FBIN("fminnm", B_MINNM), FBIN("fabd", B_ABD),
    FBIN("fscale", B_SCALE), FBIN("fmulx", B_MULX), FBIN("ftsmul", B_TSMUL),
    FBIN("frecps", B_RECPS),
    FUN("fabs", U_ABS), FUN("fneg", U_NEG), FUN("fsqrt", U_SQRT),
    FUN("frinta", U_RINTA), FUN("frinti", U_RINTI), FUN("frintm", U_RINTM),
    FUN("frintn", U_RINTN), FUN("frintp", U_RINTP), FUN("frintx", U_RINTX),
    FUN("frintz", U_RINTZ),
    { "fmla", liftFusedMultiplyAdd, 0, 0 }, { "fmls", liftFusedMultiplyAdd, 1, 0 },
    { "fnmla", liftFusedMultiplyAdd, 2, 0 }, { "fnmls", liftFusedMultiplyAdd, 3, 0 },
    { "fmad", liftFusedMultiplyAdd, 4, 0 }, { "fmsb", liftFusedMultiplyAdd, 5, 0 },
    { "fnmad", liftFusedMultiplyAdd, 6, 0 }, { "fnmsb", liftFusedMultiplyAdd, 7, 0 },
    { "fcadd", liftComplexAdd, 0, 0 },
    { "fcmla", liftComplexMultiplyAdd, 0, 0 },
    { "fcmeq", liftCompare, Op::COND_FLOAT_EQUAL, 0 },
    { "fcmne", liftCompare, Op::COND_FLOAT_NOT_EQUAL, 0 },
    { "fcmge", liftCompare, Op::COND_FLOAT_GREATER_EQUAL, 0 },
    { "fcmgt", liftCompare, Op::COND_FLOAT_GREATER, 0 },
    { "fcmle", liftCompare, Op::COND_FLOAT_LESS_EQUAL, 0 },
    { "fcmlt", liftCompare, Op::COND_FLOAT_LESS, 0 },
    { "fcmuo", liftCompare, Op::COND_FLOAT_UNORDERED, 0 },
    { "facge", liftCompare, Op::COND_FLOAT_GREATER_EQUAL, 1 },
    { "facgt", liftCompare, Op::COND_FLOAT_GREATER, 1 },
    { "faddv", liftReduce, Op::OP_FLOAT_ADD, 0 },
    { "fmaxv", liftReduce, Op::OP_FLOAT_MAXIMUM, Op::BEHAVIOR_MINMAX_PROPAGATE_NAN },
    { "fminv", liftReduce, Op::OP_FLOAT_MINIMUM, Op::BEHAVIOR_MINMAX_PROPAGATE_NAN },
    { "fmaxnmv", liftReduce, Op::OP_FLOAT_MAXIMUM, Op::BEHAVIOR_MINMAX_NUMBER },
    { "fminnmv", liftReduce, Op::OP_FLOAT_MINIMUM, Op::BEHAVIOR_MINMAX_NUMBER },
    { "fadda", liftOrderedAdd, 0, 0 },
    { "fcvt", liftConvert, 0, 0 },
    { "fcvtzs", liftConvert, 1, 0 },
    { "fcvtzu", liftConvert, 2, 0 },
    { "scvtf", liftConvert, 3, 0 },
    { "ucvtf", liftConvert, 4, 0 },
    { "fcvtx", liftConvert, 5, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSveFloat(ArmLiftContext& context)
{
    runSveTable(context, gFloatTable);
}
