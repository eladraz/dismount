/*
 * Arm64SemanticLiftSveFloat2.cpp
 *
 * Implementation file
 *
 * The A64 SVE lifters of the remaining floating point instructions: the
 * pairwise operations, the trigonometric helpers (ftmad, ftssel, fexpa), the
 * floating point immediates, the clamps, the estimates (frecpe, frsqrte,
 * frecpx), flogb, the rounding to a 32 / 64 bit integer, the conversions of
 * the SVE2 extensions, the bfloat16 arithmetic, and the widening multiply
 * adds, dot products and matrix multiplies of half precision and bfloat16.
 * Like Arm64SemanticLiftSveFloat.cpp the lanes of a predicated instruction
 * which are not active raise no exception (their operands are replaced by a
 * neutral value), and fpcr.DN is applied to the NaN results.
 *
 * Not lifted: the FP8 instructions (f1cvt, f2cvt, bf1cvt, bf2cvt, fcvtn,
 * fcvtnb, fcvtnt to bytes, fcvtzsn, fcvtzun, the byte forms of fmlal, fdot
 * and fmmla, fmlall*): the FP8 format is selected by FPMR, which the
 * architecture description does not have.
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

/////////////////////////////////////////////////////////////////////////////
// The helpers
/////////////////////////////////////////////////////////////////////////////

const ArmDecodedOperand& operandOf(SveLift& s, uint index)
{
    return s.context().getOperand(index);
}

bool isPredicateAt(SveLift& s, uint index)
{
    return (index < s.context().getOperandsCount()) &&
           SveLift::isP(operandOf(s, index));
}

// The bits of a floating point number of 'bits' bits (16, 32, 64; the
// bfloat16 with 'isBrain') for a value which is zero or a normal number
// exact in the format (the immediates, the neutral elements)
uint64 floatBits(double value, uint bits, bool isBrain = false)
{
    union { double m_double; uint64 m_bits; } converter;
    converter.m_double = value;
    const uint64 sign = converter.m_bits >> 63;
    const uint64 exponent = (converter.m_bits >> 52) & 0x7FF;
    const uint64 fraction = converter.m_bits & 0xFFFFFFFFFFFFFULL;
    uint fractionBits = 52;
    uint bias = 1023;
    if (isBrain)
    {
        fractionBits = 7;
        bias = 127;
    } else if (bits == 16)
    {
        fractionBits = 10;
        bias = 15;
    } else if (bits == 32)
    {
        fractionBits = 23;
        bias = 127;
    }
    if (exponent == 0)
        return sign << (bits - 1);
    const uint64 target = exponent - 1023 + bias;
    return (sign << (bits - 1)) | (target << fractionBits) |
           (fraction >> (52 - fractionBits));
}

SemanticOperand floatVector(SveLift& s, const SemanticOperand& integerVector,
                            uint e, bool isBrain = false)
{
    return s.bitcast(integerVector, s.floatType(e, isBrain));
}

SemanticOperand integerVectorOf(SveLift& s, const SemanticOperand& vector, uint e)
{
    return s.bitcast(vector, s.integerType(e));
}

SemanticOperand readFloat(SveLift& s, uint operandIndex, uint e, bool isBrain = false)
{
    return floatVector(s, s.readZOperand(operandIndex, e), e, isBrain);
}

SemanticOperand floatSplat(SveLift& s, double value, uint e, bool isBrain = false)
{
    return floatVector(s, s.splatInteger((int64)floatBits(value, e, isBrain), e),
                       e, isBrain);
}

SemanticOperand activeOnly(SveLift& s, const SemanticOperand& mask,
                           const SemanticOperand& value, double neutral, uint e,
                           bool isBrain = false)
{
    return s.select(mask, value, floatSplat(s, neutral, e, isBrain));
}

SemanticOperand finishNan(SveLift& s, const SemanticOperand& result)
{
    return s.applyDefaultNan(result, result.getType());
}

// Write the floating point result to the destination (operand 0), governed
// by predicate operand 'predicateIndex' (-1: unpredicated)
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
    SemanticOperand selector = s.emit(Op::OP_ADD, s.integerType(e), base,
                                      s.splatInteger(index, e));
    return s.emit(Op::OP_SHUFFLE, vector.getType(), vector, vector, selector);
}

// The bit 'lane' of the integer 'bits' of 'count' bits, as a 1 bit value
SemanticOperand laneBit(SveLift& s, const SemanticOperand& bits, uint count, uint lane)
{
    return s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                  s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, SemanticType::integer(count),
                         bits, s.integer(lane, count)));
}

// Add the bits 'flags' to fpsr when the 1 bit value 'condition' is set
void raiseFlags(SveLift& s, const SemanticOperand& condition, uint64 flags)
{
    SemanticOperand fpsr = s.context().getRegister(ARM64_REG_FPSR);
    const SemanticType type = fpsr.getType();
    SemanticOperand wide = s.emit(Op::OP_ZERO_EXTEND, type, condition);
    SemanticOperand bits = s.emit(Op::OP_MULTIPLY, type, wide, s.integer((int64)flags, type.getTotalBits()));
    s.context().move(fpsr, s.emit(Op::OP_OR, type, s.context().getRegister(ARM64_REG_FPSR), bits));
}

// fpcr is replaced by 'value' until restoreFpcr(): the bfloat16 instructions
// round to nearest, flush the denormals and produce the default NaN whatever
// fpcr says
SemanticOperand forceFpcr(SveLift& s, uint64 setBits, uint64 clearBits)
{
    SemanticOperand fpcr = s.context().getRegister(ARM64_REG_FPCR);
    SemanticOperand saved = s.emit(Op::OP_MOVE, fpcr.getType(), fpcr);
    SemanticOperand changed = s.emit(Op::OP_OR, fpcr.getType(),
        s.emit(Op::OP_AND, fpcr.getType(), saved,
               s.integer((int64)~clearBits, fpcr.getType().getTotalBits())),
        s.integer((int64)setBits, fpcr.getType().getTotalBits()));
    s.context().move(s.context().getRegister(ARM64_REG_FPCR), changed);
    return saved;
}

void restoreFpcr(SveLift& s, const SemanticOperand& saved)
{
    s.context().move(s.context().getRegister(ARM64_REG_FPCR), saved);
}

/////////////////////////////////////////////////////////////////////////////
// Pairwise: faddp, fmaxp, fminp, fmaxnmp, fminnmp
/////////////////////////////////////////////////////////////////////////////

// trn1 / trn2 of two vectors of 'e' bit lanes
SemanticOperand transpose(SveLift& s, const SemanticOperand& a, const SemanticOperand& b,
                          uint e, bool isSecond)
{
    const SemanticType type = s.integerType(e);
    SemanticOperand index = s.laneIndex(0, 1, e);
    SemanticOperand odd = s.emit(Op::OP_AND, type, index, s.splatInteger(1, e));
    SemanticOperand base = isSecond ?
        s.emit(Op::OP_OR, type, index, s.splatInteger(1, e)) :
        s.emit(Op::OP_AND, type, index, s.splatInteger(~1LL, e));
    SemanticOperand offset = s.emit(Op::OP_MULTIPLY, type, odd, s.splatInteger(s.lanes(e), e));
    return s.emit(Op::OP_SHUFFLE, type, a, b, s.emit(Op::OP_ADD, type, base, offset));
}

// m_a: 0 add, 1 maximum, 2 minimum, 3 maximum number, 4 minimum number
void liftPairwise(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand a = s.readZOperand(2, e);
    SemanticOperand b = s.readZOperand(3, e);
    SemanticOperand x = floatVector(s, transpose(s, a, b, e, false), e);
    SemanticOperand y = floatVector(s, transpose(s, a, b, e, true), e);
    x = activeOnly(s, mask, x, 0.0, e);
    y = activeOnly(s, mask, y, 0.0, e);
    SemanticOperand result;
    switch (d.m_a)
    {
    case 0: result = s.emitFloat(Op::OP_FLOAT_ADD, type, x, y); break;
    case 1: result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, x, y, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_PROPAGATE_NAN); break;
    case 2: result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, x, y, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_PROPAGATE_NAN); break;
    case 3: result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, x, y, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_NUMBER); break;
    default: result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, x, y, SemanticOperand(),
                                  Op::BEHAVIOR_MINMAX_NUMBER); break;
    }
    writeFloat(s, 1, e, finishNan(s, result));
}

/////////////////////////////////////////////////////////////////////////////
// The trigonometric helpers: ftmad, ftssel, fexpa
/////////////////////////////////////////////////////////////////////////////

// The coefficients of ftmad: the first 8 for a positive multiplier, the next
// 8 for a negative one (sin and cos series)
const uint64 gTrigHalf[16] =
{
    0x3c00, 0xb155, 0x2030, 0, 0, 0, 0, 0,
    0x3c00, 0xb800, 0x293a, 0, 0, 0, 0, 0
};
const uint64 gTrigSingle[16] =
{
    0x3f800000, 0xbe2aaaab, 0x3c088886, 0xb95008b9, 0x36369d6d, 0, 0, 0,
    0x3f800000, 0xbf000000, 0x3d2aaaa6, 0xbab60705, 0x37cd37cc, 0, 0, 0
};
const uint64 gTrigDouble[16] =
{
    0x3ff0000000000000ULL, 0xbfc5555555555543ULL, 0x3f8111111110f30cULL,
    0xbf2a01a019b92fc6ULL, 0x3ec71de351f3d22bULL, 0xbe5ae5e2b60f7b91ULL,
    0x3de5d8408868552fULL, 0,
    0x3ff0000000000000ULL, 0xbfe0000000000000ULL, 0x3fa5555555555536ULL,
    0xbf56c16c16c13a0bULL, 0x3efa01a019b1e8d8ULL, 0xbe927e4f7282f468ULL,
    0x3e21ee96d2641b13ULL, 0xbda8f76380fbb401ULL
};

// ftmad zdn, zdn, zm, #imm: zdn * |zm| + coefficient
void liftTrigMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    const uint index = (uint)operandOf(s, 3).m_immediate & 7;
    const uint64* table = (e == 16) ? gTrigHalf : ((e == 32) ? gTrigSingle : gTrigDouble);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand zdn = floatVector(s, s.readZ(reg, e), e);
    SemanticOperand zm = readFloat(s, 2, e);
    SemanticOperand isNegative = s.compareLanes(Op::COND_SIGNED_LESS, s.integerType(e),
        s.readZOperand(2, e), s.zero(s.integerType(e)));
    SemanticOperand coefficient = s.select(isNegative,
        floatVector(s, s.splatInteger((int64)table[index + 8], e), e),
        floatVector(s, s.splatInteger((int64)table[index], e), e));
    SemanticOperand result = s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, type, zdn,
                                         s.emit(Op::OP_FLOAT_ABSOLUTE, type, zm), coefficient);
    writeFloat(s, -1, e, finishNan(s, result));
}

// ftssel zd, zn, zm: 1.0 when bit 0 of zm is set, the sign flipped by bit 1
void liftTrigSelect(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    SemanticOperand n = s.readZOperand(1, e);
    SemanticOperand m = s.readZOperand(2, e);
    SemanticOperand bit0 = s.compareLanes(Op::COND_NOT_EQUAL, type,
        s.emit(Op::OP_AND, type, m, s.splatInteger(1, e)), s.zero(type));
    SemanticOperand bit1 = s.compareLanes(Op::COND_NOT_EQUAL, type,
        s.emit(Op::OP_AND, type, m, s.splatInteger(2, e)), s.zero(type));
    SemanticOperand result = s.select(bit0, s.splatInteger((int64)floatBits(1.0, e), e), n);
    SemanticOperand flipped = s.emit(Op::OP_XOR, type, result,
        s.splatInteger((int64)((uint64)1 << (e - 1)), e));
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)), s.select(bit1, flipped, result));
}

// The fraction of 2^(i / 64) (2^(i / 32) for half precision)
const uint64 gExpaHalf[32] =
{
    0x0, 0x16, 0x2d, 0x45, 0x5d, 0x75, 0x8e, 0xa8, 0xc2, 0xdc, 0xf8, 0x114,
    0x130, 0x14d, 0x16b, 0x189, 0x1a8, 0x1c8, 0x1e8, 0x209, 0x22b, 0x24e,
    0x271, 0x295, 0x2ba, 0x2e0, 0x306, 0x32e, 0x356, 0x37f, 0x3a9, 0x3d4
};
const uint64 gExpaSingle[64] =
{
    0x0, 0x164d2, 0x2cd87, 0x43a29, 0x5aac3, 0x71f62, 0x8980f, 0xa14d5, 0xb95c2,
    0xd1adf, 0xea43a, 0x1031dc, 0x11c3d3, 0x135a2b, 0x14f4f0, 0x16942d,
    0x1837f0, 0x19e046, 0x1b8d3a, 0x1d3eda, 0x1ef532, 0x20b051, 0x227043,
    0x243516, 0x25fed7, 0x27cd94, 0x29a15b, 0x2b7a3a, 0x2d583f, 0x2f3b79,
    0x3123f6, 0x3311c4, 0x3504f3, 0x36fd92, 0x38fbaf, 0x3aff5b, 0x3d08a4,
    0x3f179a, 0x412c4d, 0x4346cd, 0x45672a, 0x478d75, 0x49b9be, 0x4bec15,
    0x4e248c, 0x506334, 0x52a81e, 0x54f35b, 0x5744fd, 0x599d16, 0x5bfbb8,
    0x5e60f5, 0x60ccdf, 0x633f89, 0x65b907, 0x68396a, 0x6ac0c7, 0x6d4f30,
    0x6fe4ba, 0x728177, 0x75257d, 0x77d0df, 0x7a83b3, 0x7d3e0c
};
const uint64 gExpaDouble[64] =
{
    0x0ULL, 0x2c9a3e778061ULL, 0x59b0d3158574ULL, 0x874518759bc8ULL,
    0xb5586cf9890fULL, 0xe3ec32d3d1a2ULL, 0x11301d0125b51ULL, 0x1429aaea92de0ULL,
    0x172b83c7d517bULL, 0x1a35beb6fcb75ULL, 0x1d4873168b9aaULL, 0x2063b88628cd6ULL,
    0x2387a6e756238ULL, 0x26b4565e27cddULL, 0x29e9df51fdee1ULL, 0x2d285a6e4030bULL,
    0x306fe0a31b715ULL, 0x33c08b26416ffULL, 0x371a7373aa9cbULL, 0x3a7db34e59ff7ULL,
    0x3dea64c123422ULL, 0x4160a21f72e2aULL, 0x44e086061892dULL, 0x486a2b5c13cd0ULL,
    0x4bfdad5362a27ULL, 0x4f9b2769d2ca7ULL, 0x5342b569d4f82ULL, 0x56f4736b527daULL,
    0x5ab07dd485429ULL, 0x5e76f15ad2148ULL, 0x6247eb03a5585ULL, 0x6623882552225ULL,
    0x6a09e667f3bcdULL, 0x6dfb23c651a2fULL, 0x71f75e8ec5f74ULL, 0x75feb564267c9ULL,
    0x7a11473eb0187ULL, 0x7e2f336cf4e62ULL, 0x82589994cce13ULL, 0x868d99b4492edULL,
    0x8ace5422aa0dbULL, 0x8f1ae99157736ULL, 0x93737b0cdc5e5ULL, 0x97d829fde4e50ULL,
    0x9c49182a3f090ULL, 0xa0c667b5de565ULL, 0xa5503b23e255dULL, 0xa9e6b5579fdbfULL,
    0xae89f995ad3adULL, 0xb33a2b84f15fbULL, 0xb7f76f2fb5e47ULL, 0xbcc1e904bc1d2ULL,
    0xc199bdd85529cULL, 0xc67f12e57d14bULL, 0xcb720dcef9069ULL, 0xd072d4a07897cULL,
    0xd5818dcfba487ULL, 0xda9e603db3285ULL, 0xdfc97337b9b5fULL, 0xe502ee78b3ff6ULL,
    0xea4afa2a490daULL, 0xefa1bee615a27ULL, 0xf50765b6e4540ULL, 0xfa7c1819e90d8ULL
};

// fexpa zd, zn: the exponent from the high bits of the element and the
// fraction of the table entry the low bits select
void liftExponentialAccelerator(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.integerType(e);
    const uint indexBits = (e == 16) ? 5 : 6;
    const uint fractionBits = (e == 16) ? 10 : ((e == 32) ? 23 : 52);
    const uint exponentBits = (e == 16) ? 5 : ((e == 32) ? 8 : 11);
    SemanticOperand n = s.readZOperand(1, e);
    SemanticOperand index = s.emit(Op::OP_AND, type, n, s.splatInteger((1 << indexBits) - 1, e));
    SemanticOperand exponent = s.emit(Op::OP_AND, type,
        s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, n, s.splatInteger(indexBits, e)),
        s.splatInteger((1 << exponentBits) - 1, e));
    // The table, by a tree of selects on the bits of the index
    const uint count = 1u << indexBits;
    const uint64* table = (e == 16) ? gExpaHalf : ((e == 32) ? gExpaSingle : gExpaDouble);
    SemanticOperand level[64];
    for (uint i = 0; i < count; i++)
        level[i] = s.splatInteger((int64)table[i], e);
    for (uint bit = 0; bit < indexBits; bit++)
    {
        SemanticOperand isSet = s.compareLanes(Op::COND_NOT_EQUAL, type,
            s.emit(Op::OP_AND, type, index, s.splatInteger(1 << bit, e)), s.zero(type));
        for (uint i = 0; i < (count >> (bit + 1)); i++)
            level[i] = s.select(isSet, level[2 * i + 1], level[2 * i]);
    }
    SemanticOperand result = s.emit(Op::OP_OR, type,
        s.emit(Op::OP_SHIFT_LEFT, type, exponent, s.splatInteger(fractionBits, e)), level[0]);
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)), result);
}

/////////////////////////////////////////////////////////////////////////////
// fmov (immediate), fclamp, famax, famin
/////////////////////////////////////////////////////////////////////////////

// fmov zd, #imm | zd, pg/m, #imm
void liftMoveImmediate(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const bool isPredicated = isPredicateAt(s, 1);
    SemanticOperand value = s.splatInteger((int64)floatBits(
        operandOf(s, isPredicated ? 2 : 1).m_floatImmediate, e), e);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    if (isPredicated)
        value = s.select(s.readMaskOperand(1, e), value, s.readZ(reg, e));
    s.writeZ(reg, value);
}

// fclamp, bfclamp zd, zn, zm: min(max(zd, zn), zm). m_a: 1 bfloat16
void liftClamp(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const bool isBrain = d.m_a != 0;
    const SemanticType type = s.floatType(e, isBrain);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand value = floatVector(s, s.readZ(reg, e), e, isBrain);
    SemanticOperand low = readFloat(s, 1, e, isBrain);
    SemanticOperand high = readFloat(s, 2, e, isBrain);
    SemanticOperand result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, low, value,
        SemanticOperand(), Op::BEHAVIOR_MINMAX_NUMBER);
    result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, result, high,
        SemanticOperand(), Op::BEHAVIOR_MINMAX_NUMBER);
    writeFloat(s, -1, e, finishNan(s, result));
}

// famax, famin zdn, pg/m, zdn, zm: the maximum / minimum of the magnitudes
void liftAbsoluteMinMax(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand a = activeOnly(s, mask, readFloat(s, 2, e), 0.0, e);
    SemanticOperand b = activeOnly(s, mask, readFloat(s, 3, e), 0.0, e);
    a = s.emit(Op::OP_FLOAT_ABSOLUTE, type, a);
    b = s.emit(Op::OP_FLOAT_ABSOLUTE, type, b);
    SemanticOperand result = s.emitFloat(d.m_a == 0 ? Op::OP_FLOAT_MAXIMUM : Op::OP_FLOAT_MINIMUM,
        type, a, b, SemanticOperand(), Op::BEHAVIOR_MINMAX_PROPAGATE_NAN);
    writeFloat(s, 1, e, finishNan(s, result));
}

/////////////////////////////////////////////////////////////////////////////
// The estimates: frecpe, frsqrte, frecpx, and flogb
/////////////////////////////////////////////////////////////////////////////

// Scalar 64 bit helpers of the lane by lane lifters
SemanticOperand k64(SveLift& s, int64 value)
{
    return s.integer(value, 64);
}

SemanticOperand op64(SveLift& s, Op::Operation operation, const SemanticOperand& a,
                     const SemanticOperand& b = SemanticOperand())
{
    return s.emit(operation, SemanticType::integer(64), a, b);
}

SemanticOperand cmp64(SveLift& s, Op::Condition condition, const SemanticOperand& a,
                      const SemanticOperand& b)
{
    return s.compareScalar(condition, a, b);
}

SemanticOperand pick64(SveLift& s, const SemanticOperand& condition,
                       const SemanticOperand& a, const SemanticOperand& b)
{
    return s.emit(Op::OP_SELECT, SemanticType::integer(64), condition, a, b);
}

SemanticOperand and1(SveLift& s, const SemanticOperand& a, const SemanticOperand& b)
{
    return s.emit(Op::OP_AND, SemanticType::integer(1), a, b);
}

SemanticOperand or1(SveLift& s, const SemanticOperand& a, const SemanticOperand& b)
{
    return s.emit(Op::OP_OR, SemanticType::integer(1), a, b);
}

SemanticOperand not1(SveLift& s, const SemanticOperand& a)
{
    return s.emit(Op::OP_XOR, SemanticType::integer(1), a, s.integer(1, 1));
}

// The lane 'lane' of the integer vector, zero extended to 64 bits
SemanticOperand lane64(SveLift& s, const SemanticOperand& vector, uint lane)
{
    return s.zeroExtend(s.extractLane(vector, lane), 64);
}

// The fields of a floating point element in 64 bit scalars
struct Fields {
    SemanticOperand m_sign;       // 0 or 1
    SemanticOperand m_exponent;   // the biased exponent field
    SemanticOperand m_fraction;   // the fraction field
    SemanticOperand m_isNan, m_isInfinity, m_isZero, m_isDenormal;
};

Fields fieldsOf(SveLift& s, const SemanticOperand& x, uint e)
{
    const uint fractionBits = (e == 16) ? 10 : ((e == 32) ? 23 : 52);
    const uint64 exponentMask = (e == 16) ? 0x1F : ((e == 32) ? 0xFF : 0x7FF);
    Fields f;
    f.m_sign = op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, x, k64(s, e - 1)), k64(s, 1));
    f.m_exponent = op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, x, k64(s, fractionBits)),
                        k64(s, (int64)exponentMask));
    f.m_fraction = op64(s, Op::OP_AND, x, k64(s, (int64)(((uint64)1 << fractionBits) - 1)));
    SemanticOperand expAll = cmp64(s, Op::COND_EQUAL, f.m_exponent, k64(s, (int64)exponentMask));
    SemanticOperand expZero = cmp64(s, Op::COND_EQUAL, f.m_exponent, k64(s, 0));
    SemanticOperand fracZero = cmp64(s, Op::COND_EQUAL, f.m_fraction, k64(s, 0));
    f.m_isNan = and1(s, expAll, not1(s, fracZero));
    f.m_isInfinity = and1(s, expAll, fracZero);
    f.m_isZero = and1(s, expZero, fracZero);
    f.m_isDenormal = and1(s, expZero, not1(s, fracZero));
    return f;
}

// fpcr.FZ (FZ16 for half precision) as a 1 bit value
SemanticOperand flushControl(SveLift& s, uint e)
{
    SemanticOperand control = s.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(64),
                                     s.context().getRegister(ARM64_REG_FPCR));
    return s.emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                  op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, control, k64(s, (e == 16) ? 19 : 24)));
}

// The NaN lanes of a unary operation: the quiet NaN of the operand (default
// NaN with fpcr.DN), the flags of the operand included (invalid for a
// signaling NaN, the denormal flag of a flushed operand)
SemanticOperand nanResults(SveLift& s, const SemanticOperand& source, uint e)
{
    SemanticOperand product = s.emitFloat(Op::OP_FLOAT_MULTIPLY, s.floatType(e),
                                          floatVector(s, source, e), floatSplat(s, 1.0, e));
    return integerVectorOf(s, finishNan(s, product), e);
}

// RecipEstimate of the Arm ARM: a in 256 .. 511
SemanticOperand recipEstimate(SveLift& s, const SemanticOperand& a)
{
    SemanticOperand scaled = op64(s, Op::OP_ADD, op64(s, Op::OP_MULTIPLY, a, k64(s, 2)), k64(s, 1));
    SemanticOperand b = op64(s, Op::OP_DIVIDE_UNSIGNED, k64(s, 1 << 19), scaled);
    return op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, op64(s, Op::OP_ADD, b, k64(s, 1)), k64(s, 1));
}

// RecipSqrtEstimate of the Arm ARM: a in 128 .. 511
SemanticOperand recipSqrtEstimate(SveLift& s, const SemanticOperand& a)
{
    SemanticOperand isLow = cmp64(s, Op::COND_UNSIGNED_LESS, a, k64(s, 256));
    SemanticOperand lowValue = op64(s, Op::OP_ADD, op64(s, Op::OP_MULTIPLY, a, k64(s, 2)), k64(s, 1));
    SemanticOperand highValue = op64(s, Op::OP_MULTIPLY,
        op64(s, Op::OP_ADD, op64(s, Op::OP_AND, a, k64(s, ~1LL)), k64(s, 1)), k64(s, 2));
    SemanticOperand scaled = pick64(s, isLow, lowValue, highValue);
    // t = the largest value with scaled * t * t < 2^28, by a binary search
    SemanticOperand t = k64(s, 0);
    for (int bit = 9; bit >= 0; bit--)
    {
        SemanticOperand candidate = op64(s, Op::OP_OR, t, k64(s, 1 << bit));
        SemanticOperand square = op64(s, Op::OP_MULTIPLY, scaled,
                                      op64(s, Op::OP_MULTIPLY, candidate, candidate));
        t = pick64(s, cmp64(s, Op::COND_UNSIGNED_LESS, square, k64(s, 1 << 28)), candidate, t);
    }
    SemanticOperand b = pick64(s, cmp64(s, Op::COND_UNSIGNED_LESS, t, k64(s, 512)), k64(s, 512), t);
    return op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, op64(s, Op::OP_ADD, b, k64(s, 1)), k64(s, 1));
}

// frecpe zd, zn and frsqrte zd, zn. m_a: 0 frecpe, 1 frsqrte
void liftEstimate(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint fractionBits = (e == 16) ? 10 : ((e == 32) ? 23 : 52);
    const uint exponentBits = (e == 16) ? 5 : ((e == 32) ? 8 : 11);
    const int64 exponentMax = (1 << exponentBits) - 1;
    const bool isSqrt = d.m_a != 0;
    SemanticOperand source = s.readZOperand(1, e);
    SemanticOperand nan = nanResults(s, source, e);
    SemanticOperand flush = flushControl(s, e);
    SemanticOperand control = s.emit(Op::OP_ZERO_EXTEND, SemanticType::integer(64),
                                     s.context().getRegister(ARM64_REG_FPCR));
    SemanticOperand rounding = op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, control, k64(s, 22)), k64(s, 3));
    SemanticOperand result = s.zero(s.integerType(e));
    SemanticOperand anyDivide = s.integer(0, 1), anyOverflow = s.integer(0, 1);
    SemanticOperand anyUnderflow = s.integer(0, 1), anyInvalid = s.integer(0, 1);
    for (uint lane = 0; lane < s.lanes(e); lane++)
    {
        SemanticOperand x = lane64(s, source, lane);
        Fields f = fieldsOf(s, x, e);
        SemanticOperand signed64 = op64(s, Op::OP_SHIFT_LEFT, f.m_sign, k64(s, e - 1));
        SemanticOperand flushed = and1(s, f.m_isDenormal, flush);
        SemanticOperand isZero = or1(s, f.m_isZero, flushed);
        // The denormal operands that are not flushed, aligned: fraction as 52 bits
        SemanticOperand fraction52 = op64(s, Op::OP_SHIFT_LEFT, f.m_fraction, k64(s, 52 - fractionBits));
        SemanticOperand exponent = f.m_exponent;
        SemanticOperand topClear = cmp64(s, Op::COND_EQUAL,
            op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, fraction52, k64(s, 51)), k64(s, 1)), k64(s, 0));
        const int64 mask52 = (int64)(((uint64)1 << 52) - 1);
        SemanticOperand shifted1 = op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_LEFT, fraction52, k64(s, 1)), k64(s, mask52));
        SemanticOperand shifted2 = op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_LEFT, fraction52, k64(s, 2)), k64(s, mask52));
        SemanticOperand isExpZero = cmp64(s, Op::COND_EQUAL, exponent, k64(s, 0));
        if (!isSqrt)
        {
            // A denormal operand below 2^-(bias + 1) overflows
            SemanticOperand isTiny = and1(s, and1(s, f.m_isDenormal, not1(s, flush)),
                cmp64(s, Op::COND_UNSIGNED_LESS, f.m_fraction, k64(s, (int64)((uint64)1 << (fractionBits - 2)))));
            // The exponents at the top of the range flush to zero
            const int64 largest = exponentMax - 1;
            SemanticOperand isLarge = and1(s, flush, and1(s,
                cmp64(s, Op::COND_UNSIGNED_GREATER_EQUAL, f.m_exponent, k64(s, largest - 1)),
                cmp64(s, Op::COND_UNSIGNED_LESS, f.m_exponent, k64(s, exponentMax))));
            // The estimate path
            SemanticOperand exp2 = pick64(s, isExpZero, pick64(s, topClear, k64(s, -1), k64(s, 0)), exponent);
            SemanticOperand frac2 = pick64(s, isExpZero, pick64(s, topClear, shifted2, shifted1), fraction52);
            SemanticOperand scaled = op64(s, Op::OP_OR, k64(s, 256),
                op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, frac2, k64(s, 44)), k64(s, 255)));
            const int64 resultBase = (e == 16) ? 29 : ((e == 32) ? 253 : 2045);
            SemanticOperand resultExponent = op64(s, Op::OP_SUBTRACT, k64(s, resultBase), exp2);
            SemanticOperand estimate = recipEstimate(s, scaled);
            SemanticOperand fraction = op64(s, Op::OP_SHIFT_LEFT, op64(s, Op::OP_AND, estimate, k64(s, 255)), k64(s, 44));
            SemanticOperand isZeroExp = cmp64(s, Op::COND_EQUAL, resultExponent, k64(s, 0));
            SemanticOperand isMinusOne = cmp64(s, Op::COND_EQUAL, resultExponent, k64(s, -1));
            fraction = pick64(s, isZeroExp, op64(s, Op::OP_OR, k64(s, (int64)((uint64)1 << 51)),
                                                 op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, fraction, k64(s, 1))),
                       pick64(s, isMinusOne, op64(s, Op::OP_OR, k64(s, (int64)((uint64)1 << 50)),
                                                  op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, fraction, k64(s, 2))),
                              fraction));
            resultExponent = pick64(s, isMinusOne, k64(s, 0), resultExponent);
            SemanticOperand estimated = op64(s, Op::OP_OR, signed64,
                op64(s, Op::OP_OR, op64(s, Op::OP_SHIFT_LEFT, resultExponent, k64(s, fractionBits)),
                     op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, fraction, k64(s, 52 - fractionBits))));
            // The overflow result by the rounding mode
            SemanticOperand infinity = op64(s, Op::OP_OR, signed64, k64(s, exponentMax << fractionBits));
            SemanticOperand maximum = op64(s, Op::OP_OR, signed64,
                k64(s, ((exponentMax - 1) << fractionBits) | (int64)(((uint64)1 << fractionBits) - 1)));
            SemanticOperand toInfinity64 = pick64(s, cmp64(s, Op::COND_EQUAL, rounding, k64(s, 0)), k64(s, 1),
                pick64(s, cmp64(s, Op::COND_EQUAL, rounding, k64(s, 1)),
                       pick64(s, cmp64(s, Op::COND_EQUAL, f.m_sign, k64(s, 0)), k64(s, 1), k64(s, 0)),
                       pick64(s, cmp64(s, Op::COND_EQUAL, rounding, k64(s, 2)), f.m_sign, k64(s, 0))));
            SemanticOperand toInfinity = cmp64(s, Op::COND_NOT_EQUAL, toInfinity64, k64(s, 0));
            SemanticOperand overflowed = pick64(s, toInfinity, infinity, maximum);
            SemanticOperand r = estimated;
            r = pick64(s, isLarge, signed64, r);
            r = pick64(s, isTiny, overflowed, r);
            r = pick64(s, isZero, op64(s, Op::OP_OR, signed64, k64(s, exponentMax << fractionBits)), r);
            r = pick64(s, f.m_isInfinity, signed64, r);
            r = pick64(s, f.m_isNan, s.zeroExtend(s.extractLane(nan, lane), 64), r);
            result = s.insertLane(result, s.truncate(r, e), lane);
            anyDivide = or1(s, anyDivide, and1(s, isZero, not1(s, f.m_isNan)));
            anyOverflow = or1(s, anyOverflow, isTiny);
            anyUnderflow = or1(s, anyUnderflow, isLarge);
        } else
        {
            // Normalize a denormal operand (not flushed)
            SemanticOperand leading = op64(s, Op::OP_COUNT_LEADING_ZEROS, f.m_fraction);
            // fraction52 shifted left until its bit 51 is set, exponent decremented by the shifts
            SemanticOperand shiftCount = op64(s, Op::OP_SUBTRACT, leading, k64(s, 64 - (int64)fractionBits));
            SemanticOperand normalized = op64(s, Op::OP_AND,
                op64(s, Op::OP_SHIFT_LEFT, fraction52, op64(s, Op::OP_ADD, shiftCount, k64(s, 1))), k64(s, mask52));
            SemanticOperand denormalExponent = op64(s, Op::OP_SUBTRACT, k64(s, 0), shiftCount);
            SemanticOperand exp2 = pick64(s, isExpZero, denormalExponent, exponent);
            SemanticOperand frac2 = pick64(s, isExpZero, normalized, fraction52);
            SemanticOperand isOdd = cmp64(s, Op::COND_NOT_EQUAL, op64(s, Op::OP_AND, exp2, k64(s, 1)), k64(s, 0));
            SemanticOperand scaled = pick64(s, isOdd,
                op64(s, Op::OP_OR, k64(s, 128), op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, frac2, k64(s, 45)), k64(s, 127))),
                op64(s, Op::OP_OR, k64(s, 256), op64(s, Op::OP_AND, op64(s, Op::OP_SHIFT_RIGHT_LOGICAL, frac2, k64(s, 44)), k64(s, 255))));
            const int64 resultBase = (e == 16) ? 44 : ((e == 32) ? 380 : 3068);
            SemanticOperand resultExponent = op64(s, Op::OP_SHIFT_RIGHT_LOGICAL,
                op64(s, Op::OP_SUBTRACT, k64(s, resultBase), exp2), k64(s, 1));
            SemanticOperand estimate = recipSqrtEstimate(s, scaled);
            SemanticOperand estimated = op64(s, Op::OP_OR,
                op64(s, Op::OP_SHIFT_LEFT, resultExponent, k64(s, fractionBits)),
                op64(s, Op::OP_SHIFT_LEFT, op64(s, Op::OP_AND, estimate, k64(s, 255)), k64(s, fractionBits - 8)));
            SemanticOperand defaultNan = k64(s, (int64)(((uint64)exponentMax << fractionBits) | ((uint64)1 << (fractionBits - 1))));
            SemanticOperand negative = and1(s, cmp64(s, Op::COND_NOT_EQUAL, f.m_sign, k64(s, 0)),
                                            and1(s, not1(s, isZero), not1(s, f.m_isNan)));
            SemanticOperand r = estimated;
            r = pick64(s, f.m_isInfinity, k64(s, 0), r);
            r = pick64(s, negative, defaultNan, r);
            r = pick64(s, isZero, op64(s, Op::OP_OR, signed64, k64(s, exponentMax << fractionBits)), r);
            r = pick64(s, f.m_isNan, s.zeroExtend(s.extractLane(nan, lane), 64), r);
            result = s.insertLane(result, s.truncate(r, e), lane);
            anyDivide = or1(s, anyDivide, and1(s, isZero, not1(s, f.m_isNan)));
            anyInvalid = or1(s, anyInvalid, negative);
        }
    }
    raiseFlags(s, anyDivide, 2);
    raiseFlags(s, anyInvalid, 1);
    raiseFlags(s, anyOverflow, 0x14);
    raiseFlags(s, anyUnderflow, 8);
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)), result);
}

// frecpx zd, pg/m|z, zn: the sign, the exponent field inverted, no fraction
void liftReciprocalExponent(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint fractionBits = (e == 16) ? 10 : ((e == 32) ? 23 : 52);
    const int64 exponentMask = (e == 16) ? 0x1F : ((e == 32) ? 0xFF : 0x7FF);
    const SemanticType type = s.integerType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand source = s.select(mask, s.readZOperand(2, e), s.zero(type));
    SemanticOperand nan = nanResults(s, source, e);
    SemanticOperand exponent = s.emit(Op::OP_AND, type,
        s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, source, s.splatInteger(fractionBits, e)),
        s.splatInteger(exponentMask, e));
    SemanticOperand inverted = s.select(
        s.compareLanes(Op::COND_EQUAL, type, exponent, s.zero(type)),
        s.splatInteger(exponentMask - 1, e),
        s.emit(Op::OP_XOR, type, exponent, s.splatInteger(exponentMask, e)));
    SemanticOperand sign = s.emit(Op::OP_AND, type, source, s.splatInteger((int64)((uint64)1 << (e - 1)), e));
    SemanticOperand result = s.emit(Op::OP_OR, type, sign,
        s.emit(Op::OP_SHIFT_LEFT, type, inverted, s.splatInteger(fractionBits, e)));
    SemanticOperand isNan = s.compareLanes(Op::COND_FLOAT_UNORDERED, s.floatType(e),
        floatVector(s, source, e), floatVector(s, source, e));
    result = s.select(isNan, nan, result);
    writeFloat(s, 1, e, floatVector(s, result, e));
}

// flogb zd, pg/m|z, zn: the exponent of the operand as an integer
void liftLogBase2(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const uint fractionBits = (e == 16) ? 10 : ((e == 32) ? 23 : 52);
    const int64 bias = (e == 16) ? 15 : ((e == 32) ? 127 : 1023);
    const SemanticType type = s.integerType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand maskBits = s.bitcast(mask, SemanticType::integer(s.lanes(e)));
    SemanticOperand source = s.readZOperand(2, e);
    // The flags of the unpacking (the flushed denormal)
    nanResults(s, s.select(mask, source, s.zero(type)), e);
    SemanticOperand flush = flushControl(s, e);
    SemanticOperand result = s.zero(type);
    SemanticOperand anyInvalid = s.integer(0, 1);
    const int64 minimum = (int64)((uint64)1 << (e - 1));
    const int64 maximum = (int64)(((uint64)1 << (e - 1)) - 1);
    for (uint lane = 0; lane < s.lanes(e); lane++)
    {
        SemanticOperand active = laneBit(s, maskBits, s.lanes(e), lane);
        SemanticOperand x = lane64(s, source, lane);
        Fields f = fieldsOf(s, x, e);
        SemanticOperand flushed = and1(s, f.m_isDenormal, flush);
        SemanticOperand bad = or1(s, or1(s, f.m_isNan, f.m_isZero), flushed);
        SemanticOperand leading = op64(s, Op::OP_COUNT_LEADING_ZEROS, f.m_fraction);
        // floor(log2(fraction)) - (bias - 1 + fractionBits) for a denormal
        SemanticOperand denormal = op64(s, Op::OP_SUBTRACT,
            op64(s, Op::OP_SUBTRACT, k64(s, 63), leading), k64(s, bias - 1 + fractionBits));
        SemanticOperand normal = op64(s, Op::OP_SUBTRACT, f.m_exponent, k64(s, bias));
        SemanticOperand r = pick64(s, f.m_isDenormal, denormal, normal);
        r = pick64(s, f.m_isInfinity, k64(s, maximum), r);
        r = pick64(s, bad, k64(s, minimum), r);
        result = s.insertLane(result, s.truncate(r, e), lane);
        anyInvalid = or1(s, anyInvalid, and1(s, active, bad));
    }
    raiseFlags(s, anyInvalid, 1);
    const bool isZeroing = operandOf(s, 1).m_predicateQualifier == ArmDecodedOperand::PREDICATE_ZEROING;
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)),
             s.predicate(mask, result, s.readZ(SveLift::registerNumber(operandOf(s, 0)), e), isZeroing));
}

/////////////////////////////////////////////////////////////////////////////
// Conversions to and from the top elements: fcvtnt, fcvtlt, fcvtxnt, bfcvt,
// bfcvtnt
/////////////////////////////////////////////////////////////////////////////

SemanticType scalarFloat(uint bits, bool isBrain)
{
    if (isBrain)
        return SemanticType::floating(SemanticType::FLOAT_BFLOAT16);
    return SemanticType::floating((bits == 16) ? SemanticType::FLOAT_HALF :
                                  ((bits == 32) ? SemanticType::FLOAT_SINGLE :
                                                  SemanticType::FLOAT_DOUBLE));
}

// m_a: 0 narrow into the top half (fcvtnt, bfcvtnt), 1 lengthen from the top
// half (fcvtlt), 2 narrow into the top half rounding to odd (fcvtxnt), 3
// narrow into the bottom half (bfcvt). m_b: 1 the narrow format is bfloat16
void liftConvertTop(SveLift& s, const SveDescriptor& d)
{
    if ((s.context().getOperandsCount() != 3) || !isPredicateAt(s, 1))
    {
        // The FP8 forms
        s.undefined();
        return;
    }
    const uint destinationBits = SveLift::elementBitsOf(operandOf(s, 0));
    const uint sourceBits = SveLift::elementBitsOf(operandOf(s, 2));
    const bool isLengthen = d.m_a == 1;
    const uint container = isLengthen ? destinationBits : sourceBits;
    const uint half = container / 2;
    const uint count = s.lanes(container);
    const bool isBrain = d.m_b != 0;
    const SemanticType destinationFloat = scalarFloat(destinationBits, isBrain && !isLengthen);
    const SemanticType sourceFloat = scalarFloat(sourceBits, isBrain && isLengthen);
    SemanticOperand maskBits = s.bitcast(s.readMaskOperand(1, container), SemanticType::integer(count));
    SemanticOperand source = s.readZOperand(2, container);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand old = s.readZ(reg, container);
    const bool isZeroing = operandOf(s, 1).m_predicateQualifier == ArmDecodedOperand::PREDICATE_ZEROING;
    SemanticOperand result = s.zero(s.integerType(container));
    SemanticOperand inputFlushed = s.integer(0, 1);
    const SemanticType containerType = SemanticType::integer(container);
    for (uint lane = 0; lane < count; lane++)
    {
        SemanticOperand active = laneBit(s, maskBits, count, lane);
        SemanticOperand wide = s.extractLane(source, lane);
        SemanticOperand x = isLengthen ?
            s.emit(Op::OP_TRUNCATE, SemanticType::integer(sourceBits),
                   s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, containerType, wide, s.integer(half, container))) :
            wide;
        if (x.getType().getTotalBits() > sourceBits)
            x = s.emit(Op::OP_TRUNCATE, SemanticType::integer(sourceBits), x);
        if (!isLengthen && !isBrain && (destinationBits == 16))
        {
            // A half precision result is flushed by FZ16 but the single or
            // double operand by FZ: flush the operand here
            Fields f = fieldsOf(s, s.zeroExtend(x, 64), sourceBits);
            SemanticOperand flushIn = and1(s, f.m_isDenormal, flushControl(s, sourceBits));
            x = s.select(flushIn, s.emit(Op::OP_AND, x.getType(), x,
                s.integer((int64)((uint64)1 << (sourceBits - 1)), sourceBits)), x);
            inputFlushed = or1(s, inputFlushed, and1(s, active, flushIn));
        }
        SemanticOperand input = s.select(active, s.bitcast(x, sourceFloat), s.context().getImmediate(0, sourceFloat));
        SemanticOperand converted = s.context().newTemporary(destinationFloat);
        SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, destinationFloat, converted, input);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
        if (d.m_a == 2)
            instruction.setRoundingMode(Op::ROUND_TO_ODD);
        s.context().emit(instruction);
        SemanticOperand bits = s.bitcast(finishNan(s, converted), SemanticType::integer(destinationBits));
        SemanticOperand oldLane = s.extractLane(old, lane);
        SemanticOperand value;
        if (isLengthen || d.m_a == 3)
        {
            value = s.zeroExtend(bits, container);
        } else
        {
            SemanticOperand kept = s.emit(Op::OP_AND, containerType, oldLane,
                                          s.integer((int64)(((uint64)1 << half) - 1), container));
            value = s.emit(Op::OP_OR, containerType, kept,
                           s.emit(Op::OP_SHIFT_LEFT, containerType, s.zeroExtend(bits, container),
                                  s.integer(half, container)));
        }
        SemanticOperand inactive = isZeroing ? s.integer(0, container) : oldLane;
        result = s.insertLane(result, s.select(active, value, inactive), lane);
    }
    raiseFlags(s, inputFlushed, 0x80);
    s.writeZ(reg, result);
}

/////////////////////////////////////////////////////////////////////////////
// bfloat16 arithmetic: bfadd, bfsub, bfmul, bfmax, bfmin, bfmaxnm, bfminnm,
// bfscale, bfmla, bfmls
/////////////////////////////////////////////////////////////////////////////

// m_a: 0 add, 1 sub, 2 mul, 3 max, 4 min, 5 maxnm, 6 minnm, 7 scale
void liftBrainBinary(SveLift& s, const SveDescriptor& d)
{
    const uint e = 16;
    const SemanticType type = s.floatType(e, true);
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
    SemanticOperand a = readFloat(s, first, e, true);
    SemanticOperand b;
    if (d.m_a == 7)
    {
        const SemanticType integer = s.integerType(e);
        SemanticOperand n = s.readZOperand(first + 1, e);
        SemanticOperand high = s.splatInteger(300, e);
        SemanticOperand low = s.splatInteger(-300, e);
        n = s.select(s.compareLanes(Op::COND_SIGNED_GREATER, integer, n, high), high, n);
        n = s.select(s.compareLanes(Op::COND_SIGNED_LESS, integer, n, low), low, n);
        b = s.emit(Op::OP_SIGNED_TO_FLOAT, type, n);
    } else if (operandOf(s, first + 1).m_elementIndex >= 0)
    {
        b = floatVector(s, segmentElement(s, s.readZOperand(first + 1, e), e,
                                          operandOf(s, first + 1).m_elementIndex), e, true);
    } else
    {
        b = readFloat(s, first + 1, e, true);
    }
    if (mask.isValid())
    {
        a = activeOnly(s, mask, a, 0.0, e, true);
        b = activeOnly(s, mask, b, 0.0, e, true);
    }
    SemanticOperand result;
    switch (d.m_a)
    {
    case 0: result = s.emitFloat(Op::OP_FLOAT_ADD, type, a, b); break;
    case 1: result = s.emitFloat(Op::OP_FLOAT_SUBTRACT, type, a, b); break;
    case 2: result = s.emitFloat(Op::OP_FLOAT_MULTIPLY, type, a, b); break;
    case 3: result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, a, b, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_PROPAGATE_NAN); break;
    case 4: result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, a, b, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_PROPAGATE_NAN); break;
    case 5: result = s.emitFloat(Op::OP_FLOAT_MAXIMUM, type, a, b, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_NUMBER); break;
    case 6: result = s.emitFloat(Op::OP_FLOAT_MINIMUM, type, a, b, SemanticOperand(),
                                 Op::BEHAVIOR_MINMAX_NUMBER); break;
    default: result = s.emitFloat(Op::OP_FLOAT_SCALE, type, a, b); break;
    }
    writeFloat(s, predicateIndex, e, finishNan(s, result));
}

// bfmla, bfmls zda, pg/m, zn, zm | zda, zn, zm[i]. m_a: 1 subtract
void liftBrainFusedMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = 16;
    const SemanticType type = s.floatType(e, true);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    const bool isPredicated = isPredicateAt(s, 1);
    const uint first = isPredicated ? 2 : 1;
    SemanticOperand n = readFloat(s, first, e, true);
    SemanticOperand m;
    if (operandOf(s, first + 1).m_elementIndex >= 0)
        m = floatVector(s, segmentElement(s, s.readZOperand(first + 1, e), e,
                                          operandOf(s, first + 1).m_elementIndex), e, true);
    else
        m = readFloat(s, first + 1, e, true);
    SemanticOperand addend = floatVector(s, s.readZ(reg, e), e, true);
    if (isPredicated)
    {
        SemanticOperand mask = s.readMaskOperand(1, e);
        n = activeOnly(s, mask, n, 0.0, e, true);
        m = activeOnly(s, mask, m, 0.0, e, true);
        addend = activeOnly(s, mask, addend, 0.0, e, true);
    }
    if (d.m_a != 0)
        n = s.emit(Op::OP_FLOAT_NEGATE, type, n);
    SemanticOperand result = s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, type, n, m, addend);
    writeFloat(s, isPredicated ? 1 : -1, e, finishNan(s, result));
}

/////////////////////////////////////////////////////////////////////////////
// Widening multiply adds and dot products of half precision and bfloat16
/////////////////////////////////////////////////////////////////////////////

// Operand 'index' (a vector of 16 bit elements) as the 32 bit lanes holding
// the 16 bit numbers of the products: the bottom (or top) halves of the lanes,
// or for an indexed operand the selected element of every 128 bit segment
SemanticOperand halfBits(SveLift& s, uint index, bool isTop)
{
    const ArmDecodedOperand& operand = operandOf(s, index);
    SemanticOperand halves = s.readZOperand(index, 16);
    if (operand.m_elementIndex >= 0)
        halves = segmentElement(s, halves, 16, operand.m_elementIndex);
    SemanticOperand lanes32 = s.bitcast(halves, s.integerType(32));
    if (operand.m_elementIndex >= 0 || !isTop)
        return s.emit(Op::OP_AND, s.integerType(32), lanes32, s.splatInteger(0xFFFF, 32));
    return s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, s.integerType(32), lanes32, s.splatInteger(16, 32));
}

// bfmlalb, bfmlalt, bfmlslb, bfmlslt, fmlalb, fmlalt, fmlslb, fmlslt
// zda.s, zn.h, zm.h | zm.h[i]. m_a: bit 0 top, bit 1 subtract; m_b: 1
// half precision. Every lane is the widening fused multiply add of the
// numbers of the narrow format (the shared form of the Advanced SIMD FMLAL)
void liftWidenMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    if (SveLift::elementBitsOf(operandOf(s, 0)) != 32)
    {
        // The FP8 forms (zda.h)
        s.undefined();
        return;
    }
    const SemanticType single = s.floatType(32);
    const SemanticType narrow = SemanticType::floating(d.m_b == 0 ? SemanticType::FLOAT_BFLOAT16
                                                                  : SemanticType::FLOAT_HALF);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand n = halfBits(s, 1, (d.m_a & 1) != 0);
    SemanticOperand m = halfBits(s, 2, (d.m_a & 1) != 0);
    SemanticOperand addend = floatVector(s, s.readZ(reg, 32), 32);
    SemanticOperand result = addend;
    for (uint lane = 0; lane < s.lanes(32); lane++)
    {
        SemanticOperand x = s.bitcast(s.truncate(s.extractLane(n, lane), 16), narrow);
        SemanticOperand y = s.bitcast(s.truncate(s.extractLane(m, lane), 16), narrow);
        if ((d.m_a & 2) != 0)
            x = s.emit(Op::OP_FLOAT_NEGATE, narrow, x);
        SemanticOperand sum = s.context().newTemporary(SemanticType::floating(SemanticType::FLOAT_SINGLE));
        SemanticInstruction instruction(Op::OP_FLOAT_MULTIPLY_ADD, SemanticType::floating(SemanticType::FLOAT_SINGLE),
                                        sum, x, y, s.extractLane(addend, lane));
        instruction.setRoundingMode(Op::ROUND_DYNAMIC);
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
        s.context().emit(instruction);
        result = s.insertLane(result, sum, lane);
    }
    (void)single;
    writeFloat(s, -1, 32, result);
}

// The pair (bottom, top) of the 16 bit elements of the 32 bit lanes of operand
// 'index' as single precision numbers; an indexed operand selects the pair
// of every 128 bit segment
void widenPairs(SveLift& s, uint index, bool isBrain, SemanticOperand& bottom,
                SemanticOperand& top)
{
    const ArmDecodedOperand& operand = operandOf(s, index);
    SemanticOperand lanes = s.readZOperand(index, 32);
    if (operand.m_elementIndex >= 0)
        lanes = segmentElement(s, lanes, 32, operand.m_elementIndex);
    const SemanticType integer32 = s.integerType(32);
    const SemanticType single = s.floatType(32);
    SemanticOperand lowBits = s.emit(Op::OP_AND, integer32, lanes, s.splatInteger(0xFFFF, 32));
    SemanticOperand highBits = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integer32, lanes, s.splatInteger(16, 32));
    if (isBrain)
    {
        bottom = s.bitcast(s.emit(Op::OP_SHIFT_LEFT, integer32, lowBits, s.splatInteger(16, 32)), single);
        top = s.bitcast(s.emit(Op::OP_SHIFT_LEFT, integer32, highBits, s.splatInteger(16, 32)), single);
        return;
    }
    // Half precision: converted lane by lane, a NaN widened by its bits
    const SemanticType half = SemanticType::floating(SemanticType::FLOAT_HALF);
    const SemanticType scalar = SemanticType::floating(SemanticType::FLOAT_SINGLE);
    for (uint pass = 0; pass < 2; pass++)
    {
        const SemanticOperand& bits = pass == 0 ? lowBits : highBits;
        SemanticOperand result = s.zero(single);
        for (uint lane = 0; lane < s.lanes(32); lane++)
        {
            SemanticOperand h = s.bitcast(s.truncate(s.extractLane(bits, lane), 16), half);
            SemanticOperand converted = s.context().newTemporary(scalar);
            SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, scalar, converted, h);
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
            s.context().emit(instruction);
            result = s.insertLane(result, converted, lane);
        }
        SemanticOperand isNan = s.compareLanes(Op::COND_UNSIGNED_GREATER, integer32,
            s.emit(Op::OP_AND, integer32, bits, s.splatInteger(0x7FFF, 32)), s.splatInteger(0x7C00, 32));
        SemanticOperand widened = s.emit(Op::OP_OR, integer32,
            s.emit(Op::OP_OR, integer32,
                s.emit(Op::OP_SHIFT_LEFT, integer32,
                       s.emit(Op::OP_AND, integer32, bits, s.splatInteger(0x8000, 32)), s.splatInteger(16, 32)),
                s.splatInteger(0x7F800000, 32)),
            s.emit(Op::OP_SHIFT_LEFT, integer32,
                   s.emit(Op::OP_AND, integer32, bits, s.splatInteger(0x3FF, 32)), s.splatInteger(13, 32)));
        SemanticOperand value = s.bitcast(s.select(isNan, widened, s.bitcast(result, integer32)), single);
        if (pass == 0)
            bottom = value;
        else
            top = value;
    }
}

// The 16 bit elements (bottom, top) of the 32 bit lanes of operand 'index' (the
// selected pair of every 128 bit segment for an indexed one), as 32 bit lanes
void halfPairBits(SveLift& s, uint index, SemanticOperand& bottom, SemanticOperand& top)
{
    const ArmDecodedOperand& operand = operandOf(s, index);
    SemanticOperand lanes = s.readZOperand(index, 32);
    if (operand.m_elementIndex >= 0)
        lanes = segmentElement(s, lanes, 32, operand.m_elementIndex);
    bottom = s.emit(Op::OP_AND, s.integerType(32), lanes, s.splatInteger(0xFFFF, 32));
    top = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, s.integerType(32), lanes, s.splatInteger(16, 32));
}

// The product of the two pairs and the sum into the accumulator, the way of
// the standard behavior of the bfloat16 and half precision dot products:
// round to nearest, flush to zero, default NaN. 'accumulator' is the single
// precision lanes of zda
SemanticOperand roundedOperation(SveLift& s, Op::Operation operation, const SemanticType& type,
                                 const SemanticOperand& a, const SemanticOperand& b,
                                 Op::RoundingMode mode, bool raisesFlags)
{
    SemanticOperand result = s.context().newTemporary(type);
    SemanticInstruction instruction(operation, type, result, a, b);
    instruction.setRoundingMode(mode);
    if (raisesFlags)
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    s.context().emit(instruction);
    return result;
}

// The operation rounded to odd, except that an overflow gives the infinity
SemanticOperand oddOperation(SveLift& s, Op::Operation operation, const SemanticType& type,
                             const SemanticOperand& a, const SemanticOperand& b)
{
    SemanticOperand odd = roundedOperation(s, operation, type, a, b, Op::ROUND_TO_ODD, false);
    SemanticOperand nearest = roundedOperation(s, operation, type, a, b, Op::ROUND_NEAREST_EVEN, false);
    SemanticOperand magnitude = s.emit(Op::OP_AND, s.integerType(32), s.bitcast(nearest, s.integerType(32)),
                                       s.splatInteger(0x7FFFFFFF, 32));
    SemanticOperand isInfinity = s.compareLanes(Op::COND_EQUAL, s.integerType(32), magnitude,
                                                s.splatInteger(0x7F800000, 32));
    return s.select(isInfinity, nearest, odd);
}

SemanticOperand dotStep(SveLift& s, const SemanticOperand& accumulator,
                        const SemanticOperand& a0, const SemanticOperand& a1,
                        const SemanticOperand& b0, const SemanticOperand& b1)
{
    const SemanticType single = s.floatType(32);
    SemanticOperand p0 = oddOperation(s, Op::OP_FLOAT_MULTIPLY, single, a0, b0);
    SemanticOperand p1 = oddOperation(s, Op::OP_FLOAT_MULTIPLY, single, a1, b1);
    SemanticOperand product = finishNan(s, oddOperation(s, Op::OP_FLOAT_ADD, single, p0, p1));
    return finishNan(s, oddOperation(s, Op::OP_FLOAT_ADD, single, accumulator, product));
}

// fpcr bits: RMode (23:22), FZ (24), DN (25)
const uint64 gFpcrRounding = 3ULL << 22;
const uint64 gFpcrFlushAndDefaultNan = (1ULL << 24) | (1ULL << 25);

// bfdot, fdot zda.s, zn.h, zm.h | zm.h[i]. m_a: 1 half precision
void liftDotProduct(SveLift& s, const SveDescriptor& d)
{
    if ((SveLift::elementBitsOf(operandOf(s, 1)) != 16) ||
        (SveLift::elementBitsOf(operandOf(s, 0)) != 32))
    {
        // The FP8 forms
        s.undefined();
        return;
    }
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand a0, a1, b0, b1;
    widenPairs(s, 1, d.m_a == 0, a0, a1);
    widenPairs(s, 2, d.m_a == 0, b0, b1);
    SemanticOperand accumulator = floatVector(s, s.readZ(reg, 32), 32);
    if (d.m_a != 0)
    {
        // Half precision: the two products summed with one rounding, then added to the
        // accumulator, with the current fpcr
        const SemanticType single = s.floatType(32);
        // The lanes with a NaN element compute zeros (their flags are the signaling NaN ones)
        const SemanticType integerType32 = s.integerType(32);
        const SemanticType laneMask = SemanticType::mask(s.lanes(32));
        const SemanticOperand* elements[4] = { &a0, &a1, &b0, &b1 };
        SemanticOperand anyNan = s.compareLanes(Op::COND_NOT_EQUAL, integerType32, s.zero(integerType32), s.zero(integerType32));
        SemanticOperand anySignaling = anyNan;
        for (int i = 0; i < 4; i++)
        {
            SemanticOperand bits = s.bitcast(*elements[i], integerType32);
            SemanticOperand isNan = s.compareLanes(Op::COND_UNSIGNED_GREATER, integerType32,
                s.emit(Op::OP_AND, integerType32, bits, s.splatInteger(0x7FFFFFFF, 32)), s.splatInteger(0x7F800000, 32));
            SemanticOperand isQuietBit = s.compareLanes(Op::COND_NOT_EQUAL, integerType32,
                s.emit(Op::OP_AND, integerType32, bits, s.splatInteger(0x400000, 32)), s.zero(integerType32));
            anyNan = s.emit(Op::OP_OR, laneMask, anyNan, isNan);
            anySignaling = s.emit(Op::OP_OR, laneMask, anySignaling, s.emit(Op::OP_AND_NOT, laneMask, isQuietBit, isNan));
        }
        // The products of the half precision numbers themselves (the flush to zero of the
        // denormal operands is the one of FPCR.FZ16): the shared widening multiply add
        SemanticOperand lowA, highA, lowB, highB;
        halfPairBits(s, 1, lowA, highA);
        halfPairBits(s, 2, lowB, highB);
        const SemanticType half = SemanticType::floating(SemanticType::FLOAT_HALF);
        const SemanticType scalarSingle = SemanticType::floating(SemanticType::FLOAT_SINGLE);
        const SemanticOperand zeroBits = s.zero(integerType32);
        lowA = s.select(anyNan, zeroBits, lowA);
        highA = s.select(anyNan, zeroBits, highA);
        lowB = s.select(anyNan, zeroBits, lowB);
        highB = s.select(anyNan, zeroBits, highB);
        SemanticOperand pair = s.zero(single);
        for (uint lane = 0; lane < s.lanes(32); lane++)
        {
            SemanticOperand x0 = s.bitcast(s.truncate(s.extractLane(lowA, lane), 16), half);
            SemanticOperand x1 = s.bitcast(s.truncate(s.extractLane(highA, lane), 16), half);
            SemanticOperand y0 = s.bitcast(s.truncate(s.extractLane(lowB, lane), 16), half);
            SemanticOperand y1 = s.bitcast(s.truncate(s.extractLane(highB, lane), 16), half);
            // x1 * y1 is exact in single precision: adding -0.0 keeps it
            SemanticOperand product = s.context().newTemporary(scalarSingle);
            SemanticInstruction first(Op::OP_FLOAT_MULTIPLY_ADD, scalarSingle, product, x1, y1,
                                      s.bitcast(s.integer(0x80000000LL, 32), scalarSingle));
            first.setRoundingMode(Op::ROUND_DYNAMIC);
            first.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
            s.context().emit(first);
            SemanticOperand sum = s.context().newTemporary(scalarSingle);
            SemanticInstruction second(Op::OP_FLOAT_MULTIPLY_ADD, scalarSingle, sum, x0, y0, product);
            second.setRoundingMode(Op::ROUND_DYNAMIC);
            second.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
            s.context().emit(second);
            pair = s.insertLane(pair, sum, lane);
        }
        raiseFlags(s, s.compareScalar(Op::COND_NOT_EQUAL, s.bitcast(anySignaling, SemanticType::integer(s.lanes(32))),
                                      s.integer(0, s.lanes(32))), 1);
        // A NaN among the four elements: a signaling one first, in the order a0, a1, b0, b1
        // (the default NaN of inf * 0 is the result only without NaN elements)
        const SemanticType integer32 = s.integerType(32);
        const SemanticOperand* operands[4] = { &a0, &a1, &b0, &b1 };
        SemanticOperand chosen = s.bitcast(pair, integer32);
        SemanticOperand quiet = s.splatInteger(0x400000, 32);
        const SemanticType maskType = SemanticType::mask(s.lanes(32));
        for (int pass = 0; pass < 2; pass++)
        {
            for (int i = 3; i >= 0; i--)
            {
                SemanticOperand bits = s.bitcast(*operands[i], integer32);
                SemanticOperand magnitude = s.emit(Op::OP_AND, integer32, bits, s.splatInteger(0x7FFFFFFF, 32));
                SemanticOperand isNan = s.compareLanes(Op::COND_UNSIGNED_GREATER, integer32, magnitude,
                                                       s.splatInteger(0x7F800000, 32));
                SemanticOperand isQuiet = s.compareLanes(Op::COND_NOT_EQUAL, integer32,
                    s.emit(Op::OP_AND, integer32, bits, quiet), s.zero(integer32));
                SemanticOperand wanted = pass == 0 ? s.emit(Op::OP_AND, maskType, isNan, isQuiet)
                                                   : s.emit(Op::OP_AND_NOT, maskType, isQuiet, isNan);
                chosen = s.select(wanted, s.emit(Op::OP_OR, integer32, bits, quiet), chosen);
            }
        }
        pair = s.bitcast(chosen, single);
        SemanticOperand r = s.emitFloat(Op::OP_FLOAT_ADD, single, accumulator, pair);
        writeFloat(s, -1, 32, finishNan(s, r));
        return;
    }
    SemanticOperand saved = forceFpcr(s, gFpcrFlushAndDefaultNan, gFpcrRounding);
    SemanticOperand result = dotStep(s, accumulator, a0, a1, b0, b1);
    restoreFpcr(s, saved);
    writeFloat(s, -1, 32, result);
}

/////////////////////////////////////////////////////////////////////////////
// frint32z, frint32x, frint64z, frint64x, scvtflt, ucvtflt
/////////////////////////////////////////////////////////////////////////////

// zd, pg/m|z, zn: round to an integral value which fits the 32 / 64 bit
// signed integer; the NaNs, the infinities and the values out of range give
// the most negative integer (as a number) and the invalid operation.
// m_a: 32 or 64; m_b: 1 exact (x), the current rounding mode and the inexact flag
void liftRoundToInteger(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    SemanticOperand mask = s.readMaskOperand(1, e);
    SemanticOperand x = activeOnly(s, mask, readFloat(s, 2, e), 0.0, e);
    SemanticOperand rounded = s.context().newTemporary(type);
    SemanticInstruction instruction(Op::OP_FLOAT_ROUND, type, rounded, x);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    if (d.m_b == 0)
    {
        instruction.setRoundingMode(Op::ROUND_TOWARD_ZERO);
        instruction.setKind(Op::FLOAT_ROUND_SUPPRESS_INEXACT);
    } else
    {
        instruction.setKind(Op::FLOAT_ROUND_SIGNAL_INEXACT);
    }
    s.context().emit(instruction);
    // The limit 2^(n - 1), exact in both formats
    const double limit = (d.m_a == 32) ? 2147483648.0 : 9223372036854775808.0;
    SemanticOperand high = floatSplat(s, limit, e);
    SemanticOperand low = floatSplat(s, -limit, e);
    SemanticOperand inRange = s.emit(Op::OP_AND, SemanticType::mask(s.lanes(e)),
        s.compareLanes(Op::COND_FLOAT_GREATER_EQUAL, type, rounded, low),
        s.compareLanes(Op::COND_FLOAT_LESS, type, rounded, high));
    SemanticOperand outOfRange = s.emit(Op::OP_AND_NOT, SemanticType::mask(s.lanes(e)), inRange, mask);
    raiseFlags(s, s.compareScalar(Op::COND_NOT_EQUAL, s.bitcast(outOfRange, SemanticType::integer(s.lanes(e))),
                                  s.integer(0, s.lanes(e))), 1);
    writeFloat(s, 1, e, s.select(inRange, rounded, low));
}

// scvtflt, ucvtflt zd.t, zn.u: the top element of every pair of source
// elements as a signed / unsigned integer converted. m_a: 1 unsigned
void liftConvertIntegerTop(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    const SemanticType type = s.floatType(e);
    const SemanticType integer = s.integerType(e);
    SemanticOperand lanes = s.readZOperand(1, e);
    SemanticOperand value;
    if (d.m_a != 0)
        value = s.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integer, lanes, s.splatInteger(e / 2, e));
    else
        value = s.emit(Op::OP_SHIFT_RIGHT_ARITHMETIC, integer, lanes, s.splatInteger(e / 2, e));
    SemanticOperand result = s.context().newTemporary(type);
    SemanticInstruction instruction(d.m_a != 0 ? Op::OP_UNSIGNED_TO_FLOAT : Op::OP_SIGNED_TO_FLOAT,
                                    type, result, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    s.context().emit(instruction);
    writeFloat(s, -1, e, result);
}

/////////////////////////////////////////////////////////////////////////////
// Matrix multiply accumulate: fmmla, bfmmla
/////////////////////////////////////////////////////////////////////////////

// fmmla zda.s|d, zn, zm: a 2 x 2 matrix of every 128 (256) bit segment, the
// product of zn by the transpose of zm added to zda. The sums are chained
// fused multiply adds
void liftMatrixMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    if ((e != 32) && (e != 64))
    {
        // The FP8 forms
        s.undefined();
        return;
    }
    if (SveLift::elementBitsOf(operandOf(s, 1)) != e)
    {
        s.undefined();
        return;
    }
    if ((e == 64) && (s.vectorBits() < 256))
    {
        s.context().trap(Op::TRAP_UNDEFINED, ARM_TRAP_UNDEFINED);
        return;
    }
    const SemanticType type = s.floatType(e);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand a = readFloat(s, 1, e);
    SemanticOperand b = readFloat(s, 2, e);
    SemanticOperand acc = floatVector(s, s.readZ(reg, e), e);
    SemanticOperand result = acc;
    for (uint segment = 0; segment < s.lanes(e) / 4; segment++)
    {
        const uint base = segment * 4;
        for (uint i = 0; i < 2; i++)
        {
            for (uint j = 0; j < 2; j++)
            {
                SemanticOperand sum = s.extractLane(acc, base + 2 * i + j);
                for (uint k = 0; k < 2; k++)
                    sum = s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, type.getElementType(),
                                      s.extractLane(a, base + 2 * i + k),
                                      s.extractLane(b, base + 2 * j + k), sum);
                result = s.insertLane(result, finishNan(s, sum), base + 2 * i + j);
            }
        }
    }
    writeFloat(s, -1, e, result);
}

// bfmmla zda.s, zn.h, zm.h: a 2 x 4 by 4 x 2 matrix product of every 128 bit
// segment, in two steps of the bfloat16 dot product
void liftBrainMatrixMultiplyAdd(SveLift& s, const SveDescriptor& d)
{
    const SemanticType single = s.floatType(32);
    const uint reg = SveLift::registerNumber(operandOf(s, 0));
    SemanticOperand lanesA = s.readZOperand(1, 32);
    SemanticOperand lanesB = s.readZOperand(2, 32);
    const SemanticType integer32 = s.integerType(32);
    // The bottom and top elements of every 32 bit lane as single precision numbers
    SemanticOperand aBottom = s.bitcast(s.emit(Op::OP_SHIFT_LEFT, integer32, lanesA, s.splatInteger(16, 32)), single);
    SemanticOperand aTop = s.bitcast(s.emit(Op::OP_AND, integer32, lanesA, s.splatInteger(0xFFFF0000LL, 32)), single);
    SemanticOperand bBottom = s.bitcast(s.emit(Op::OP_SHIFT_LEFT, integer32, lanesB, s.splatInteger(16, 32)), single);
    SemanticOperand bTop = s.bitcast(s.emit(Op::OP_AND, integer32, lanesB, s.splatInteger(0xFFFF0000LL, 32)), single);
    SemanticOperand acc = floatVector(s, s.readZ(reg, 32), 32);
    SemanticOperand result = acc;
    SemanticOperand saved = forceFpcr(s, gFpcrFlushAndDefaultNan, gFpcrRounding);
    for (uint segment = 0; segment < s.lanes(32) / 4; segment++)
    {
        const uint base = segment * 4;
        for (uint i = 0; i < 2; i++)
        {
            for (uint j = 0; j < 2; j++)
            {
                SemanticOperand sum = s.extractLane(acc, base + 2 * i + j);
                // Row i of the first matrix is the lanes 2i and 2i + 1 (four elements)
                for (uint k = 0; k < 2; k++)
                {
                    SemanticOperand step = dotStep(s, s.splat(sum, single),
                        s.splat(s.extractLane(aBottom, base + 2 * i + k), single),
                        s.splat(s.extractLane(aTop, base + 2 * i + k), single),
                        s.splat(s.extractLane(bBottom, base + 2 * j + k), single),
                        s.splat(s.extractLane(bTop, base + 2 * j + k), single));
                    sum = s.extractLane(step, 0);
                }
                result = s.insertLane(result, sum, base + 2 * i + j);
            }
        }
    }
    restoreFpcr(s, saved);
    writeFloat(s, -1, 32, result);
}

/////////////////////////////////////////////////////////////////////////////
// frsqrts
/////////////////////////////////////////////////////////////////////////////

// frsqrts zd, zn, zm: (3 - zn * zm) / 2 fused (the step of the shared ARM
// behavior, 1.5 for infinity * 0)
void liftReciprocalSqrtStep(SveLift& s, const SveDescriptor&)
{
    const uint e = SveLift::elementBitsOf(operandOf(s, 0));
    SemanticOperand a = readFloat(s, 1, e);
    SemanticOperand b = readFloat(s, 2, e);
    writeFloat(s, -1, e, s.emitFloat(Op::OP_FLOAT_MULTIPLY_ADD, s.floatType(e), a, b, SemanticOperand(),
                                     Op::BEHAVIOR_STEP_RECIPROCAL_SQRT));
}

/////////////////////////////////////////////////////////////////////////////
// fcvtzsn, fcvtzun: two vectors of floating point numbers converted to
// integers of half the size, interleaved
/////////////////////////////////////////////////////////////////////////////

// zd.t, {zn.u, zn+1.u}: element 2i of the result is the conversion of element
// i of the first vector, element 2i + 1 the one of the second (round toward
// zero, saturated, a NaN is 0). m_a: 1 unsigned
void liftConvertToIntegerNarrow(SveLift& s, const SveDescriptor& d)
{
    const ArmDecodedOperand& list = operandOf(s, 1);
    const uint sourceBits = SveLift::elementBitsOf(list);
    const uint destinationBits = SveLift::elementBitsOf(operandOf(s, 0));
    if ((list.m_kind != ArmDecodedOperand::KIND_REGISTER_LIST) || (list.m_count != 2) ||
        (destinationBits * 2 != sourceBits))
    {
        s.undefined();
        return;
    }
    const uint first = SveLift::registerNumber(list);
    const uint stride = (list.m_stride == 0) ? 1 : list.m_stride;
    const SemanticType sourceFloat = scalarFloat(sourceBits, false);
    const SemanticType destinationInteger = SemanticType::integer(destinationBits);
    SemanticOperand sources[2];
    sources[0] = s.readZ(first, sourceBits);
    sources[1] = s.readZ((first + stride) & 31, sourceBits);
    SemanticOperand result = s.zero(s.integerType(destinationBits));
    for (uint lane = 0; lane < s.lanes(sourceBits); lane++)
    {
        for (uint half = 0; half < 2; half++)
        {
            SemanticOperand converted = s.context().newTemporary(destinationInteger);
            SemanticInstruction instruction(d.m_a != 0 ? Op::OP_FLOAT_TO_UNSIGNED : Op::OP_FLOAT_TO_SIGNED,
                                            destinationInteger, converted,
                                            s.bitcast(s.extractLane(sources[half], lane), sourceFloat));
            instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
            instruction.setRoundingMode(Op::ROUND_TOWARD_ZERO);
            instruction.setBehavior(Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO);
            s.context().emit(instruction);
            result = s.insertLane(result, converted, 2 * lane + half);
        }
    }
    s.writeZ(SveLift::registerNumber(operandOf(s, 0)), result);
}

// @@HANDLERS

/////////////////////////////////////////////////////////////////////////////
// The table
/////////////////////////////////////////////////////////////////////////////

const SveDescriptor gTable[] =
{
    { "faddp", liftPairwise, 0, 0 },
    { "fmaxp", liftPairwise, 1, 0 },
    { "fminp", liftPairwise, 2, 0 },
    { "fmaxnmp", liftPairwise, 3, 0 },
    { "fminnmp", liftPairwise, 4, 0 },
    { "ftmad", liftTrigMultiplyAdd, 0, 0 },
    { "ftssel", liftTrigSelect, 0, 0 },
    { "fexpa", liftExponentialAccelerator, 0, 0 },
    { "fmov", liftMoveImmediate, 0, 0 },
    { "fclamp", liftClamp, 0, 0 },
    { "bfclamp", liftClamp, 1, 0 },
    { "famax", liftAbsoluteMinMax, 0, 0 },
    { "famin", liftAbsoluteMinMax, 1, 0 },
    { "frecpe", liftEstimate, 0, 0 },
    { "frsqrte", liftEstimate, 1, 0 },
    { "frecpx", liftReciprocalExponent, 0, 0 },
    { "flogb", liftLogBase2, 0, 0 },
    { "fcvtnt", liftConvertTop, 0, 0 },
    { "fcvtlt", liftConvertTop, 1, 0 },
    { "fcvtxnt", liftConvertTop, 2, 0 },
    { "bfcvt", liftConvertTop, 3, 1 },
    { "bfcvtnt", liftConvertTop, 0, 1 },
    { "bfadd", liftBrainBinary, 0, 0 },
    { "bfsub", liftBrainBinary, 1, 0 },
    { "bfmul", liftBrainBinary, 2, 0 },
    { "bfmax", liftBrainBinary, 3, 0 },
    { "bfmin", liftBrainBinary, 4, 0 },
    { "bfmaxnm", liftBrainBinary, 5, 0 },
    { "bfminnm", liftBrainBinary, 6, 0 },
    { "bfscale", liftBrainBinary, 7, 0 },
    { "bfmla", liftBrainFusedMultiplyAdd, 0, 0 },
    { "bfmls", liftBrainFusedMultiplyAdd, 1, 0 },
    { "bfmlalb", liftWidenMultiplyAdd, 0, 0 },
    { "bfmlalt", liftWidenMultiplyAdd, 1, 0 },
    { "bfmlslb", liftWidenMultiplyAdd, 2, 0 },
    { "bfmlslt", liftWidenMultiplyAdd, 3, 0 },
    { "fmlalb", liftWidenMultiplyAdd, 0, 1 },
    { "fmlalt", liftWidenMultiplyAdd, 1, 1 },
    { "fmlslb", liftWidenMultiplyAdd, 2, 1 },
    { "fmlslt", liftWidenMultiplyAdd, 3, 1 },
    { "bfdot", liftDotProduct, 0, 0 },
    { "fdot", liftDotProduct, 1, 0 },
    { "frint32z", liftRoundToInteger, 32, 0 },
    { "frint32x", liftRoundToInteger, 32, 1 },
    { "frint64z", liftRoundToInteger, 64, 0 },
    { "frint64x", liftRoundToInteger, 64, 1 },
    { "scvtflt", liftConvertIntegerTop, 0, 0 },
    { "ucvtflt", liftConvertIntegerTop, 1, 0 },
    { "fmmla", liftMatrixMultiplyAdd, 0, 0 },
    { "bfmmla", liftBrainMatrixMultiplyAdd, 0, 0 },
    { "frsqrts", liftReciprocalSqrtStep, 0, 0 },
    { "fcvtzsn", liftConvertToIntegerNarrow, 0, 0 },
    { "fcvtzun", liftConvertToIntegerNarrow, 1, 0 },
    { NULL, NULL, 0, 0 }
};

} // namespace

void armdis::a64::liftSveFloat2(ArmLiftContext& context)
{
    runSveTable(context, gTable);
}
