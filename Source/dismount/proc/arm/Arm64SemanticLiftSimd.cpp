/*
 * Arm64SemanticLiftSimd.cpp
 *
 * Implementation file
 *
 * The dispatcher of the A64 scalar floating point, Advanced SIMD and
 * cryptographic lifters and the helpers they share (SimdLift), see
 * Arm64SemanticSimd.h.
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
#include "dismount/proc/arm/Arm64SemanticSimd.h"

using namespace armdis;

typedef SemanticOperation Op;
typedef SimdLift::Val Val;

namespace {

// The size of an element of an arrangement in bits, 0 if it is not one
uint arrangementElementBits(uint arrangement)
{
    switch (arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_8B:
    case ArmDecodedOperand::ARRANGEMENT_16B:
    case ArmDecodedOperand::ARRANGEMENT_B:
        return 8;
    case ArmDecodedOperand::ARRANGEMENT_4H:
    case ArmDecodedOperand::ARRANGEMENT_8H:
    case ArmDecodedOperand::ARRANGEMENT_H:
        return 16;
    case ArmDecodedOperand::ARRANGEMENT_2S:
    case ArmDecodedOperand::ARRANGEMENT_4S:
    case ArmDecodedOperand::ARRANGEMENT_S:
        return 32;
    case ArmDecodedOperand::ARRANGEMENT_1D:
    case ArmDecodedOperand::ARRANGEMENT_2D:
    case ArmDecodedOperand::ARRANGEMENT_D:
        return 64;
    case ArmDecodedOperand::ARRANGEMENT_1Q:
    case ArmDecodedOperand::ARRANGEMENT_Q:
        return 128;
    // The groups of the dot products: the element is the group
    case ArmDecodedOperand::ARRANGEMENT_4B:
    case ArmDecodedOperand::ARRANGEMENT_2H:
        return 32;
    case ArmDecodedOperand::ARRANGEMENT_2B:
        return 16;
    default:
        return 0;
    }
}

// The size of a vector arrangement in bits (8B is 64), 0 for the others
uint arrangementBits(uint arrangement)
{
    switch (arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_8B:
    case ArmDecodedOperand::ARRANGEMENT_4H:
    case ArmDecodedOperand::ARRANGEMENT_2S:
    case ArmDecodedOperand::ARRANGEMENT_1D:
        return 64;
    case ArmDecodedOperand::ARRANGEMENT_16B:
    case ArmDecodedOperand::ARRANGEMENT_8H:
    case ArmDecodedOperand::ARRANGEMENT_4S:
    case ArmDecodedOperand::ARRANGEMENT_2D:
    case ArmDecodedOperand::ARRANGEMENT_1Q:
        return 128;
    default:
        return 0;
    }
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// SimdLift
/////////////////////////////////////////////////////////////////////////////

SimdLift::SimdLift(ArmLiftContext& context) :
    m_context(context),
    m_name(context.getInstruction().getEntry()->m_mnemonic),
    m_encoding(context.getInstruction().getEncoding())
{
}

bool SimdLift::is(const char* name) const
{
    return strcmp(m_name, name) == 0;
}

bool SimdLift::startsWith(const char* prefix) const
{
    return strncmp(m_name, prefix, strlen(prefix)) == 0;
}

uint32 SimdLift::bits(uint high, uint low) const
{
    return (m_encoding >> low) & ((uint32)((1ull << (high - low + 1)) - 1));
}

SimdShape SimdLift::shape(uint index) const
{
    const ArmDecodedOperand& o = operand(index);
    SimdShape s;
    s.m_isVector = false;
    s.m_isElement = false;
    s.m_isScalar = false;
    s.m_elementBits = 0;
    s.m_lanes = 1;
    s.m_totalBits = 0;
    CHECK(o.m_kind == ArmDecodedOperand::KIND_REGISTER);
    const uint element = arrangementElementBits(o.m_arrangement);
    if (o.m_elementIndex >= 0)
    {
        s.m_isElement = true;
        s.m_elementBits = element;
        s.m_totalBits = element;
    } else if (arrangementBits(o.m_arrangement) != 0)
    {
        s.m_isVector = true;
        s.m_elementBits = element;
        s.m_totalBits = arrangementBits(o.m_arrangement);
        s.m_lanes = s.m_totalBits / element;
    } else
    {
        s.m_isScalar = true;
        s.m_elementBits = m_context.getArchitecture()->getRegister(
                                                    o.m_register).m_bits;
        s.m_totalBits = s.m_elementBits;
    }
    return s;
}

bool SimdLift::isScalar(uint index) const
{
    return shape(index).m_isScalar;
}

uint SimdLift::registerNumber(uint index) const
{
    return registerNumberOf(operand(index).m_register);
}

uint SimdLift::registerNumberOf(uint id) const
{
    if ((id >= ARM64_REG_V0) && (id <= ARM64_REG_V31))
        return id - ARM64_REG_V0;
    if ((id >= ARM64_REG_Q0) && (id <= ARM64_REG_Q31))
        return id - ARM64_REG_Q0;
    if ((id >= ARM64_REG_D0) && (id <= ARM64_REG_D31))
        return id - ARM64_REG_D0;
    if ((id >= ARM64_REG_S0) && (id <= ARM64_REG_S31))
        return id - ARM64_REG_S0;
    if ((id >= ARM64_REG_H0) && (id <= ARM64_REG_H31))
        return id - ARM64_REG_H0;
    if ((id >= ARM64_REG_B0) && (id <= ARM64_REG_B31))
        return id - ARM64_REG_B0;
    if ((id >= ARM64_REG_Z0) && (id <= ARM64_REG_Z31))
        return id - ARM64_REG_Z0;
    CHECK_FAIL();
}

/////////////////////////////////////////////////////////////////////////////
// Types
/////////////////////////////////////////////////////////////////////////////

SemanticType SimdLift::I(uint bits)
{
    return SemanticType::integer(bits);
}

SemanticType SimdLift::V(uint elementBits, uint lanes)
{
    if (lanes == 1)
        return SemanticType::integer(elementBits);
    return SemanticType::vector(SemanticType::integer(elementBits), lanes);
}

SemanticType::FloatFormat SimdLift::format(uint bits)
{
    switch (bits)
    {
    case 16: return SemanticType::FLOAT_HALF;
    case 32: return SemanticType::FLOAT_SINGLE;
    default: return SemanticType::FLOAT_DOUBLE;
    }
}

SemanticType SimdLift::F(uint bits)
{
    return SemanticType::floating(format(bits));
}

SemanticType SimdLift::FV(uint elementBits, uint lanes)
{
    if (lanes == 1)
        return F(elementBits);
    return SemanticType::vector(F(elementBits), lanes);
}

SemanticType SimdLift::M(uint lanes)
{
    if (lanes == 1)
        return SemanticType::integer(1);
    return SemanticType::mask(lanes);
}

/////////////////////////////////////////////////////////////////////////////
// Values
/////////////////////////////////////////////////////////////////////////////

Val SimdLift::imm(int64 value, const SemanticType& type)
{
    return m_context.getImmediate(value, type);
}

Val SimdLift::op(Op::Operation operation, const SemanticType& type,
                 const Val& a, const Val& b, const Val& c)
{
    return m_context.emit(operation, type, a, b, c);
}

Val SimdLift::cast(const SemanticType& type, const Val& value)
{
    if (value.getType() == type)
        return value;
    CHECK(value.getType().getTotalBits() == type.getTotalBits());
    return m_context.emit(Op::OP_BITCAST, type, value);
}

Val SimdLift::zext(uint bits, const Val& value)
{
    if (value.getType().getTotalBits() == bits)
        return value;
    return m_context.emit(Op::OP_ZERO_EXTEND, I(bits), value);
}

Val SimdLift::sext(uint bits, const Val& value)
{
    if (value.getType().getTotalBits() == bits)
        return value;
    return m_context.emit(Op::OP_SIGN_EXTEND, I(bits), value);
}

Val SimdLift::trunc(uint bits, const Val& value)
{
    if (value.getType().getTotalBits() == bits)
        return value;
    return m_context.emit(Op::OP_TRUNCATE, I(bits), value);
}

Val SimdLift::lane(const Val& vector, uint index)
{
    const SemanticType type = vector.getType();
    CHECK(type.isVector() && (index < type.getLanes()));
    return m_context.emit(Op::OP_EXTRACT, I(type.getElementBits()), vector,
                          imm(index, I(32)));
}

Val SimdLift::setLane(const Val& vector, const Val& scalar, uint index)
{
    const SemanticType type = vector.getType();
    CHECK(type.isVector() && (index < type.getLanes()));
    return m_context.emit(Op::OP_INSERT, type, vector, scalar,
                          imm(index, I(32)));
}

Val SimdLift::splat(const SemanticType& type, const Val& scalar)
{
    return m_context.emit(Op::OP_BROADCAST, type, scalar);
}

Val SimdLift::splat(const SemanticType& type, int64 value)
{
    return splat(type, imm(value, I(type.getElementBits())));
}

Val SimdLift::maskToLanes(const Val& mask, uint elementBits, uint lanes)
{
    if (lanes == 1)
    {
        // A boolean to an all ones / all zeros integer
        return m_context.emit(Op::OP_SELECT, I(elementBits), mask,
                              imm(-1, I(elementBits)), imm(0, I(elementBits)));
    }
    const SemanticType type = V(elementBits, lanes);
    return m_context.emit(Op::OP_SELECT, type, mask, splat(type, -1),
                          splat(type, 0));
}

Val SimdLift::compare(Op::Condition condition, const SemanticType& type,
                      const Val& a, const Val& b)
{
    const uint lanes = type.isVector() ? type.getLanes() : 1;
    Val result = m_context.newTemporary(M(lanes));
    SemanticInstruction instruction(Op::OP_COMPARE, type, result, a, b);
    instruction.setCondition(condition);
    m_context.emit(instruction);
    return result;
}

Val SimdLift::compareLanes(Op::Condition condition, uint elementBits,
                           uint lanes, const Val& a, const Val& b)
{
    return maskToLanes(compare(condition, V(elementBits, lanes), a, b),
                       elementBits, lanes);
}

Val SimdLift::select(const Val& condition, const Val& a, const Val& b)
{
    return m_context.emit(Op::OP_SELECT, a.getType(), condition, a, b);
}

Val SimdLift::low64(const Val& value)
{
    return trunc(64, value);
}

Val SimdLift::high64(const Val& value)
{
    return m_context.emit(Op::OP_EXTRACT, I(64), value, imm(64, I(32)));
}

Val SimdLift::join64(const Val& low, const Val& high)
{
    return m_context.emit(Op::OP_CONCAT, I(128), low, high);
}

Val SimdLift::nonZero(const Val& value)
{
    Val v = value;
    const uint total = v.getType().getTotalBits();
    if (v.getType().isVector() || (v.getType().getKind() != SemanticType::KIND_INTEGER))
        v = cast(I(total), v);
    if (total > 64)
    {
        CHECK(total == 128);
        v = op(Op::OP_OR, I(64), low64(v), high64(v));
    }
    return compare(Op::COND_NOT_EQUAL, I(v.getType().getTotalBits()), v,
                   imm(0, I(v.getType().getTotalBits())));
}

/////////////////////////////////////////////////////////////////////////////
// Registers
/////////////////////////////////////////////////////////////////////////////

Val SimdLift::readV(uint number)
{
    return m_context.getRegister(ARM64_REG_V0 + number);
}

void SimdLift::writeV(uint number, const Val& value)
{
    Val v = value;
    if (v.getType().getKind() != SemanticType::KIND_INTEGER || v.getType().isVector())
        v = cast(I(v.getType().getTotalBits()), v);
    v = zext(128, v);
    m_context.move(m_context.getRegister(ARM64_REG_V0 + number), v);
}

/////////////////////////////////////////////////////////////////////////////
// Floating point and the saturation flag
/////////////////////////////////////////////////////////////////////////////

Val SimdLift::fp(Op::Operation operation, const Val& a, const Val& b,
                 const Val& c, Op::Behavior behavior,
                 Op::RoundingMode rounding, uint kind)
{
    const SemanticType integerType = a.getType();
    const uint lanes = integerType.isVector() ? integerType.getLanes() : 1;
    const uint elementBits = integerType.isVector() ?
                    integerType.getElementBits() : integerType.getTotalBits();
    const SemanticType floatType = FV(elementBits, lanes);
    Val fa = cast(floatType, a);
    Val fb = b.isValid() ? cast(floatType, b) : Val();
    Val fc = c.isValid() ? cast(floatType, c) : Val();
    Val result = m_context.newTemporary(floatType);
    SemanticInstruction instruction(operation, floatType, result, fa, fb, fc);
    instruction.setRoundingMode(rounding);
    instruction.setBehavior(behavior);
    instruction.setKind(kind);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return cast(integerType, result);
}

Val SimdLift::fpCompare(Op::Condition condition, const Val& a, const Val& b,
                        Op::Behavior behavior)
{
    const SemanticType integerType = a.getType();
    const uint lanes = integerType.isVector() ? integerType.getLanes() : 1;
    const uint elementBits = integerType.isVector() ?
                    integerType.getElementBits() : integerType.getTotalBits();
    const SemanticType floatType = FV(elementBits, lanes);
    Val result = m_context.newTemporary(M(lanes));
    SemanticInstruction instruction(Op::OP_COMPARE, floatType, result,
                                    cast(floatType, a), cast(floatType, b));
    instruction.setCondition(condition);
    instruction.setBehavior(behavior);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return result;
}

// The immediate source 1 of the fixed point conversions: the number of
// fraction bits (none for the plain integers)
Val SimdLift::fractionOperand(uint fractionBits)
{
    if (fractionBits == 0)
        return Val();
    return m_context.getImmediate((int64)fractionBits,
                                  SemanticType::integer(32));
}

Val SimdLift::fpToInteger(const Val& a, uint integerBits, bool isSigned,
                          Op::RoundingMode rounding, uint fractionBits)
{
    const SemanticType sourceType = a.getType();
    const uint lanes = sourceType.isVector() ? sourceType.getLanes() : 1;
    const uint elementBits = sourceType.isVector() ?
                    sourceType.getElementBits() : sourceType.getTotalBits();
    const SemanticType floatType = FV(elementBits, lanes);
    const SemanticType resultType = V(integerBits, lanes);
    Val result = m_context.newTemporary(resultType);
    SemanticInstruction instruction(isSigned ? Op::OP_FLOAT_TO_SIGNED :
                                               Op::OP_FLOAT_TO_UNSIGNED,
                                    resultType, result, cast(floatType, a),
                                    fractionOperand(fractionBits));
    instruction.setRoundingMode(rounding);
    instruction.setBehavior(Op::BEHAVIOR_CONVERT_SATURATE_NAN_ZERO);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return result;
}

Val SimdLift::integerToFp(const Val& a, uint floatBits, bool isSigned,
                          uint fractionBits)
{
    const SemanticType sourceType = a.getType();
    const uint lanes = sourceType.isVector() ? sourceType.getLanes() : 1;
    const SemanticType floatType = FV(floatBits, lanes);
    Val result = m_context.newTemporary(floatType);
    SemanticInstruction instruction(isSigned ? Op::OP_SIGNED_TO_FLOAT :
                                               Op::OP_UNSIGNED_TO_FLOAT,
                                    floatType, result, a,
                                    fractionOperand(fractionBits));
    instruction.setRoundingMode(Op::ROUND_DYNAMIC);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return cast(V(floatBits, lanes), result);
}

Val SimdLift::fpConvert(const Val& a, uint toBits, Op::RoundingMode rounding)
{
    const SemanticType sourceType = a.getType();
    const uint lanes = sourceType.isVector() ? sourceType.getLanes() : 1;
    const uint fromBits = sourceType.isVector() ?
                    sourceType.getElementBits() : sourceType.getTotalBits();
    const SemanticType fromType = FV(fromBits, lanes);
    const SemanticType toType = FV(toBits, lanes);
    Val result = m_context.newTemporary(toType);
    SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, toType, result,
                                    cast(fromType, a));
    instruction.setRoundingMode(rounding);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return cast(V(toBits, lanes), result);
}

Val SimdLift::fpConvertFormat(const Val& a, SemanticType::FloatFormat fromFormat,
                              SemanticType::FloatFormat toFormat,
                              Op::RoundingMode rounding)
{
    const SemanticType sourceType = a.getType();
    const uint lanes = sourceType.isVector() ? sourceType.getLanes() : 1;
    const uint fromBits = sourceType.isVector() ?
                    sourceType.getElementBits() : sourceType.getTotalBits();
    const uint toBits = (toFormat == SemanticType::FLOAT_DOUBLE) ? 64 :
                        ((toFormat == SemanticType::FLOAT_SINGLE) ? 32 : 16);
    const SemanticType fromType = lanes == 1 ? SemanticType::floating(fromFormat) :
        SemanticType::vector(SemanticType::floating(fromFormat), lanes);
    const SemanticType toType = lanes == 1 ? SemanticType::floating(toFormat) :
        SemanticType::vector(SemanticType::floating(toFormat), lanes);
    (void)fromBits;
    Val result = m_context.newTemporary(toType);
    SemanticInstruction instruction(Op::OP_FLOAT_CONVERT, toType, result,
                                    cast(fromType, a));
    instruction.setRoundingMode(rounding);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return cast(V(toBits, lanes), result);
}

Val SimdLift::bfloatDot(const Val& addend, const Val& a, const Val& b)
{
    const SemanticType pairType = SemanticType::vector(
                        SemanticType::floating(SemanticType::FLOAT_BFLOAT16), 2);
    const SemanticType resultType = F(32);
    Val result = m_context.newTemporary(resultType);
    SemanticInstruction instruction(Op::OP_DOT_PRODUCT, resultType, result,
                                    cast(pairType, a), cast(pairType, b),
                                    cast(resultType, addend));
    m_context.emit(instruction);
    return cast(I(32), result);
}

Val SimdLift::fpMultiplyAddWidening(const Val& a, const Val& b, const Val& c,
                                    SemanticType::FloatFormat narrowFormat,
                                    uint wideBits)
{
    const SemanticType narrowType = SemanticType::floating(narrowFormat);
    const SemanticType wideType = F(wideBits);
    Val result = m_context.newTemporary(wideType);
    SemanticInstruction instruction(Op::OP_FLOAT_MULTIPLY_ADD, wideType, result,
                                    cast(narrowType, a), cast(narrowType, b),
                                    cast(wideType, c));
    instruction.setRoundingMode(Op::ROUND_DYNAMIC);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS);
    m_context.emit(instruction);
    return cast(I(wideBits), result);
}

void SimdLift::setFpsrBit(uint bit, const Val& condition)
{
    Val status = m_context.getRegister(ARM64_REG_FPSR);
    Val flag = op(Op::OP_SHIFT_LEFT, I(32), zext(32, condition),
                  imm(bit, I(32)));
    m_context.move(m_context.getRegister(ARM64_REG_FPSR),
                   op(Op::OP_OR, I(32), status, flag));
}

void SimdLift::setSaturation(const Val& isSaturated)
{
    setFpsrBit(27, isSaturated);
}

/////////////////////////////////////////////////////////////////////////////
// The dispatcher
/////////////////////////////////////////////////////////////////////////////

void armdis::liftArm64Simd(ArmLiftContext& context)
{
    SimdLift s(context);
    // The memory and the crypto groups first: they check the mnemonic before
    // they look at the operands, the floating point lifters read the shape of
    // the first register operand (which a register list is not)
    if (liftSimdMemory(s) || liftSimdCrypto(s) || liftSimdFloat(s) ||
        liftSimdInteger(s))
        return;
    a64::undefined(context);
}
