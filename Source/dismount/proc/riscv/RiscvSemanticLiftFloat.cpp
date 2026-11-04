/*
 * RiscvSemanticLiftFloat.cpp
 *
 * Implementation file
 *
 * The lifters of the floating point instructions: F, D, Zfh, Zfhmin, Zfa and
 * Zfbfmin (see RiscvSemanticInternal.h). The registers are 64 bits wide and
 * a narrower value is NaN boxed (RiscvLiftContext::readFloat / writeFloat).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/except/assert.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

namespace {

// The floating point type of a format
SemanticType floatType(uint format)
{
    return SemanticType::floating((SemanticType::FloatFormat)format);
}

// The rounding mode of the instruction: its last operand, dyn if it has
// none. The reserved modes raise the illegal instruction exception
uint getRounding(RiscvLiftContext& context)
{
    return context.getRoundingMode(context.getOperandsCount() - 1);
}

// Add a floating point operation which raises exceptions and return its
// result
SemanticOperand emitFloat(RiscvLiftContext& context, Op::Operation operation,
                          const SemanticType& type, uint rounding,
                          Op::Behavior behavior,
                          const SemanticOperand& source0,
                          const SemanticOperand& source1 = SemanticOperand(),
                          const SemanticOperand& source2 = SemanticOperand())
{
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, source0, source1,
                                    source2);
    instruction.setRoundingMode((Op::RoundingMode)rounding);
    instruction.setBehavior(behavior);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    return result;
}

// Return the bits of a float as an integer of the same size
SemanticOperand floatBits(RiscvLiftContext& context,
                          const SemanticOperand& value)
{
    return makeBitcast(context, value,
                       integerType(value.getType().getTotalBits()));
}

// Raise the flags of fflags: fflags |= flags (a 5 bit constant or value)
void raiseFlags(RiscvLiftContext& context, const SemanticOperand& flags)
{
    SemanticOperand fflags = context.getRegister(RISCV_REG_FFLAGS);
    context.move(fflags, context.emit(Op::OP_OR, integerType(5), fflags,
                                      flags));
}

} // namespace

void liftFloatLoad(RiscvLiftContext& context)
{
    const uint format = (uint)context.getParameter();
    const uint bits = floatType(format).getTotalBits();
    context.writeFloat(0, context.load(context.getAddress(1), integerType(bits)));
}

void liftFloatStore(RiscvLiftContext& context)
{
    // The low bits of the register as they are: a store doesn't look at the
    // NaN boxing
    const uint bits = floatType((uint)context.getParameter()).getTotalBits();
    SemanticOperand value = context.getRegister(
                                    context.getOperand(0).m_register);
    context.store(context.getAddress(1), makeTruncate(context, value, bits));
}

void liftFloatArithmetic(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const uint rounding = getRounding(context);
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand b = context.readFloat(2, type);
    context.writeFloat(0, emitFloat(context, (Op::Operation)(parameter >> 8),
                                    type, rounding, Op::BEHAVIOR_DEFAULT, a, b));
}

void liftFloatSquareRoot(RiscvLiftContext& context)
{
    const SemanticType type = floatType((uint)context.getParameter());
    const uint rounding = getRounding(context);
    SemanticOperand a = context.readFloat(1, type);
    context.writeFloat(0, emitFloat(context, Op::OP_FLOAT_SQUARE_ROOT, type,
                                    rounding, Op::BEHAVIOR_DEFAULT, a));
}

void liftFloatFused(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const uint kind = (uint)(parameter >> 8);
    const uint rounding = getRounding(context);
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand b = context.readFloat(2, type);
    SemanticOperand c = context.readFloat(3, type);
    // fmadd: a * b + c, fmsub: a * b - c, fnmsub: -(a * b) + c,
    // fnmadd: -(a * b) - c
    if ((kind == 2) || (kind == 3))
        a = context.emit(Op::OP_FLOAT_NEGATE, type, a);
    if ((kind == 1) || (kind == 3))
        c = context.emit(Op::OP_FLOAT_NEGATE, type, c);
    context.writeFloat(0, emitFloat(context, Op::OP_FLOAT_MULTIPLY_ADD, type,
                                    rounding, Op::BEHAVIOR_DEFAULT, a, b, c));
}

void liftFloatSign(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const uint kind = (uint)(parameter >> 8);
    const uint bits = type.getTotalBits();
    const SemanticType integer = integerType(bits);
    SemanticOperand a = floatBits(context, context.readFloat(1, type));
    SemanticOperand b = floatBits(context, context.readFloat(2, type));
    SemanticOperand sign = makeConstant(context, (int64)((uint64)1 << (bits - 1)),
                                        integer);
    SemanticOperand signOfB = context.emit(Op::OP_AND, integer, b, sign);
    SemanticOperand result;
    switch (kind)
    {
    case 0:
        // fsgnj: the sign of b
        result = context.emit(Op::OP_OR, integer,
                    context.emit(Op::OP_AND_NOT, integer, sign, a), signOfB);
        break;
    case 1:
        // fsgnjn: the opposite sign of b
        result = context.emit(Op::OP_OR, integer,
                    context.emit(Op::OP_AND_NOT, integer, sign, a),
                    context.emit(Op::OP_XOR, integer, signOfB, sign));
        break;
    default:
        // fsgnjx: the sign of a xor the sign of b
        result = context.emit(Op::OP_XOR, integer, a, signOfB);
        break;
    }
    context.writeFloat(0, result);
}

void liftFloatMinMax(RiscvLiftContext& context)
{
    // The parameter is the format | (operation << 8) | (1 << 16) for the
    // propagating minimum / maximum of Zfa
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand b = context.readFloat(2, type);
    const Op::Behavior behavior = ((parameter >> 16) != 0) ?
            Op::BEHAVIOR_MINMAX_PROPAGATE_NAN : Op::BEHAVIOR_MINMAX_NUMBER;
    context.writeFloat(0, emitFloat(context,
            (Op::Operation)((parameter >> 8) & 0xFF), type, Op::ROUND_DYNAMIC,
            behavior, a, b));
}

void liftFloatCompare(RiscvLiftContext& context)
{
    // The parameter is the format | (kind << 8): feq (0), flt (1), fle (2),
    // fltq (3) and fleq (4). feq, fltq and fleq signal only for a signaling
    // NaN, flt and fle for any NaN
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const uint kind = (uint)(parameter >> 8);
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand b = context.readFloat(2, type);
    Op::Condition condition = Op::COND_FLOAT_EQUAL;
    if ((kind == 1) || (kind == 3))
        condition = Op::COND_FLOAT_LESS;
    else if ((kind == 2) || (kind == 4))
        condition = Op::COND_FLOAT_LESS_EQUAL;
    SemanticOperand result = context.newTemporary(integerType(1));
    SemanticInstruction instruction(Op::OP_COMPARE, type, result, a, b);
    instruction.setCondition(condition);
    instruction.setBehavior(((kind == 1) || (kind == 2)) ?
                    Op::BEHAVIOR_COMPARE_SIGNALING : Op::BEHAVIOR_COMPARE_QUIET);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    context.write(0, makeZeroExtend(context, result, context.getXlen()));
}

void liftFloatClassify(RiscvLiftContext& context)
{
    const SemanticType type = floatType((uint)context.getParameter());
    SemanticOperand a = context.readFloat(1, type);
    context.write(0, context.emit(Op::OP_FLOAT_CLASSIFY,
                                  integerType(context.getXlen()), a));
}

void liftFloatToInteger(RiscvLiftContext& context)
{
    // The parameter is the format | (unsigned << 8) | (bits << 16)
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const bool isUnsigned = ((parameter >> 8) & 1) != 0;
    const uint bits = (uint)((parameter >> 16) & 0xFF);
    const uint rounding = getRounding(context);
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand result = emitFloat(context,
            isUnsigned ? Op::OP_FLOAT_TO_UNSIGNED : Op::OP_FLOAT_TO_SIGNED,
            integerType(bits), rounding,
            Op::BEHAVIOR_CONVERT_SATURATE_NAN_MAXIMUM, a);
    // (the operand is the float, the operation the integer)
    context.writeSignExtended(0, result);
}

void liftIntegerToFloat(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const bool isUnsigned = ((parameter >> 8) & 1) != 0;
    const uint bits = (uint)((parameter >> 16) & 0xFF);
    const uint rounding = getRounding(context);
    SemanticOperand a = context.read(1, integerType(bits));
    context.writeFloat(0, emitFloat(context,
            isUnsigned ? Op::OP_UNSIGNED_TO_FLOAT : Op::OP_SIGNED_TO_FLOAT,
            type, rounding, Op::BEHAVIOR_DEFAULT, a));
}

void liftFloatConvert(RiscvLiftContext& context)
{
    const uint64 parameter = context.getParameter();
    const SemanticType toType = floatType((uint)(parameter & 0xFF));
    const SemanticType fromType = floatType((uint)((parameter >> 8) & 0xFF));
    const uint rounding = getRounding(context);
    SemanticOperand a = context.readFloat(1, fromType);
    context.writeFloat(0, emitFloat(context, Op::OP_FLOAT_CONVERT, toType,
                                    rounding, Op::BEHAVIOR_DEFAULT, a));
}

void liftFloatMoveToInteger(RiscvLiftContext& context)
{
    // The raw bits of the register, sign extended
    const uint bits = floatType((uint)context.getParameter()).getTotalBits();
    SemanticOperand value = context.getRegister(
                                    context.getOperand(1).m_register);
    context.writeSignExtended(0, makeTruncate(context, value, bits));
}

void liftFloatMoveFromInteger(RiscvLiftContext& context)
{
    const uint bits = floatType((uint)context.getParameter()).getTotalBits();
    context.writeFloat(0, context.read(1, integerType(bits)));
}

void liftFloatLoadImmediate(RiscvLiftContext& context)
{
    const SemanticType type = floatType((uint)context.getParameter());
    context.writeFloat(0, context.read(1, type));
}

void liftFloatRound(RiscvLiftContext& context)
{
    // The parameter is the format | (1 << 8) for froundnx, which signals
    // inexact
    const uint64 parameter = context.getParameter();
    const SemanticType type = floatType((uint)(parameter & 0xFF));
    const uint rounding = getRounding(context);
    SemanticOperand a = context.readFloat(1, type);
    SemanticOperand result = context.newTemporary(type);
    SemanticInstruction instruction(Op::OP_FLOAT_ROUND, type, result, a);
    instruction.setRoundingMode((Op::RoundingMode)rounding);
    instruction.setKind(((parameter >> 8) != 0) ?
                        Op::FLOAT_ROUND_SIGNAL_INEXACT :
                        Op::FLOAT_ROUND_SUPPRESS_INEXACT);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    context.emit(instruction);
    context.writeFloat(0, result);
}

void liftFloatConvertModular(RiscvLiftContext& context)
{
    // fcvtmod.w.d (round toward zero): the integer part of the double modulo
    // 2^32, as a signed word. NaN and infinity give 0 and invalid; a value
    // outside of the range of the word raises invalid too, inexact is
    // raised for a fraction
    const SemanticType type = integerType(64);
    SemanticOperand bits = floatBits(context,
            context.readFloat(1, floatType(SemanticType::FLOAT_DOUBLE)));
    SemanticOperand exponent = context.emit(Op::OP_AND, type,
            context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, bits,
                         makeConstant(context, 52, type)),
            makeConstant(context, 0x7FF, type));
    SemanticOperand fraction = context.emit(Op::OP_AND, type, bits,
                         makeConstant(context, (int64)maskOf(52), type));
    SemanticOperand isNormal = makeCompare(context, Op::COND_NOT_EQUAL, type,
                         exponent, makeConstant(context, 0, type));
    SemanticOperand significand = context.emit(Op::OP_OR, type, fraction,
            makeSelect(context, type, isNormal,
                       makeConstant(context, (int64)((uint64)1 << 52), type),
                       makeConstant(context, 0, type)));
    SemanticOperand isSpecial = makeCompare(context, Op::COND_EQUAL, type,
                         exponent, makeConstant(context, 0x7FF, type));
    // The value is significand * 2^(exponent - 1075) (a denormal has the
    // exponent of the smallest normal: 1 - 1075)
    SemanticOperand biased = makeSelect(context, type, isNormal, exponent,
                                        makeConstant(context, 1, type));
    SemanticOperand shift = context.emit(Op::OP_SUBTRACT, type, biased,
                                         makeConstant(context, 1075, type));
    SemanticOperand isLeft = makeCompare(context, Op::COND_SIGNED_GREATER_EQUAL,
                         type, shift, makeConstant(context, 0, type));
    // (the shifts of the IR by the size or more give 0)
    SemanticOperand left = context.emit(Op::OP_SHIFT_LEFT, type, significand,
                                        shift);
    SemanticOperand negated = context.emit(Op::OP_NEGATE, type, shift);
    SemanticOperand right = context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type,
                                         significand, negated);
    SemanticOperand magnitude = makeSelect(context, type, isLeft, left, right);
    // The lost fraction: the bits shifted out
    SemanticOperand back = context.emit(Op::OP_SHIFT_LEFT, type, right, negated);
    SemanticOperand isInexact = context.emit(Op::OP_AND, integerType(1),
            makeCompare(context, Op::COND_NOT_EQUAL, type, back, significand),
            context.emit(Op::OP_XOR, integerType(1), isLeft,
                         makeConstant(context, 1, integerType(1))));
    // The range: the truncated magnitude must be below 2^31, or 2^31 for a
    // negative value; a left shift beyond 64 bits overflowed
    SemanticOperand isNegative = makeCompare(context, Op::COND_NOT_EQUAL, type,
            context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, type, bits,
                         makeConstant(context, 63, type)),
            makeConstant(context, 0, type));
    SemanticOperand limit = makeSelect(context, type, isNegative,
            makeConstant(context, (int64)((uint64)1 << 31), type),
            makeConstant(context, (int64)(((uint64)1 << 31) - 1), type));
    SemanticOperand isTooBig = makeCompare(context, Op::COND_UNSIGNED_GREATER,
                         type, magnitude, limit);
    // A value of 2^52 or more (the shift is left) is out of range whatever
    // the bits that are left of it are
    SemanticOperand isInvalid = context.emit(Op::OP_OR, integerType(1),
            context.emit(Op::OP_OR, integerType(1), isSpecial, isTooBig),
            isLeft);
    // The modulo result: the two's complement of the low 32 bits
    SemanticOperand signedValue = makeSelect(context, type, isNegative,
            context.emit(Op::OP_NEGATE, type, magnitude), magnitude);
    SemanticOperand result = makeSelect(context, integerType(32), isSpecial,
            makeConstant(context, 0, integerType(32)),
            makeTruncate(context, signedValue, 32));
    // fflags: nv is bit 4, nx is bit 0
    SemanticOperand flags = context.emit(Op::OP_OR, integerType(5),
            makeSelect(context, integerType(5), isInvalid,
                       makeConstant(context, 0x10, integerType(5)),
                       makeConstant(context, 0, integerType(5))),
            makeSelect(context, integerType(5),
                       context.emit(Op::OP_AND, integerType(1), isInexact,
                            context.emit(Op::OP_XOR, integerType(1), isInvalid,
                                         makeConstant(context, 1, integerType(1)))),
                       makeConstant(context, 1, integerType(5)),
                       makeConstant(context, 0, integerType(5))));
    raiseFlags(context, flags);
    context.writeSignExtended(0, result);
}

void liftFloatMovePair(RiscvLiftContext& context)
{
    if (context.getParameter() == 0)
    {
        // fmvh.x.d: the high word of the register
        SemanticOperand value = context.getRegister(
                                    context.getOperand(1).m_register);
        context.write(0, context.emit(Op::OP_TRUNCATE, integerType(32),
                context.emit(Op::OP_SHIFT_RIGHT_LOGICAL, integerType(64), value,
                             makeConstant(context, 32, integerType(64)))));
    } else
    {
        // fmvp.d.x: rs1 is the low word and rs2 the high one
        SemanticOperand low = context.read(1, integerType(32));
        SemanticOperand high = context.read(2, integerType(32));
        context.writeFloat(0, context.emit(Op::OP_CONCAT, integerType(64),
                                           low, high));
    }
}

} // namespace riscvdis
