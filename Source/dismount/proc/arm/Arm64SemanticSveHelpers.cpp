/*
 * Arm64SemanticSveHelpers.cpp
 *
 * Implementation file
 *
 * The helpers of the SVE lifters: the vector length, the types, the z and p
 * registers and the predication. See Arm64SemanticSve.h.
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

SveLift::SveLift(ArmLiftContext& context) :
    m_context(context),
    m_vectorBits(128)
{
    const Arm64Architecture* architecture =
        (const Arm64Architecture*)context.getArchitecture().getPointer();
    m_vectorBits = architecture->getVectorBits();
}

SemanticType SveLift::integerType(uint elementBits) const
{
    return SemanticType::vector(SemanticType::integer(elementBits),
                                m_vectorBits / elementBits);
}

SemanticType SveLift::floatType(uint elementBits, bool isBrain) const
{
    SemanticType::FloatFormat format = SemanticType::FLOAT_DOUBLE;
    if (isBrain)
        format = SemanticType::FLOAT_BFLOAT16;
    else if (elementBits == 16)
        format = SemanticType::FLOAT_HALF;
    else if (elementBits == 32)
        format = SemanticType::FLOAT_SINGLE;
    return SemanticType::vector(SemanticType::floating(format),
                                m_vectorBits / elementBits);
}

SemanticType SveLift::maskType(uint elementBits) const
{
    return SemanticType::mask(m_vectorBits / elementBits);
}

SemanticType SveLift::wholeType() const
{
    return SemanticType::integer(m_vectorBits);
}

SemanticType SveLift::predicateType() const
{
    return SemanticType::integer(m_vectorBits / 8);
}

uint SveLift::elementBitsOf(const ArmDecodedOperand& operand)
{
    switch (operand.m_arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_B: return 8;
    case ArmDecodedOperand::ARRANGEMENT_H: return 16;
    case ArmDecodedOperand::ARRANGEMENT_S: return 32;
    case ArmDecodedOperand::ARRANGEMENT_D: return 64;
    case ArmDecodedOperand::ARRANGEMENT_Q: return 128;
    default: return 0;
    }
}

bool SveLift::isZ(const ArmDecodedOperand& operand)
{
    return ((operand.m_kind == ArmDecodedOperand::KIND_REGISTER) ||
            (operand.m_kind == ArmDecodedOperand::KIND_REGISTER_LIST)) &&
           (operand.m_register >= ARM64_REG_Z0) &&
           (operand.m_register <= ARM64_REG_Z31);
}

bool SveLift::isP(const ArmDecodedOperand& operand)
{
    return (operand.m_kind == ArmDecodedOperand::KIND_REGISTER) &&
           (operand.m_register >= ARM64_REG_P0) &&
           (operand.m_register <= ARM64_REG_P15);
}

uint SveLift::registerNumber(const ArmDecodedOperand& operand)
{
    if ((operand.m_register >= ARM64_REG_Z0) &&
        (operand.m_register <= ARM64_REG_Z31))
        return operand.m_register - ARM64_REG_Z0;
    if ((operand.m_register >= ARM64_REG_P0) &&
        (operand.m_register <= ARM64_REG_P15))
        return operand.m_register - ARM64_REG_P0;
    return operand.m_register;
}

SemanticOperand SveLift::emit(SemanticOperation::Operation operation,
                              const SemanticType& type,
                              const SemanticOperand& a,
                              const SemanticOperand& b,
                              const SemanticOperand& c)
{
    return m_context.emit(operation, type, a, b, c);
}

SemanticOperand SveLift::emitBehavior(SemanticOperation::Operation operation,
                                      const SemanticType& type,
                                      const SemanticOperand& a,
                                      const SemanticOperand& b,
                                      SemanticOperation::Behavior behavior,
                                      const SemanticOperand& c)
{
    SemanticOperand result = m_context.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, a, b, c);
    instruction.setBehavior(behavior);
    m_context.emit(instruction);
    return result;
}

SemanticOperand SveLift::compareLanes(SemanticOperation::Condition condition,
                                      const SemanticType& type,
                                      const SemanticOperand& a,
                                      const SemanticOperand& b)
{
    SemanticOperand result = m_context.newTemporary(
                                    SemanticType::mask(type.getLanes()));
    SemanticInstruction instruction(Op::OP_COMPARE, type, result, a, b);
    instruction.setCondition(condition);
    if (type.getKind() == SemanticType::KIND_FLOAT)
        instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    m_context.emit(instruction);
    return result;
}

SemanticOperand SveLift::compareScalar(SemanticOperation::Condition condition,
                                       const SemanticOperand& a,
                                       const SemanticOperand& b)
{
    return compare(m_context, condition, a.getType(), a, b);
}

SemanticOperand SveLift::maskToVector(const SemanticOperand& mask,
                                      uint elementBits)
{
    return select(mask, splatInteger(1, elementBits),
                  zero(integerType(elementBits)));
}

SemanticOperand SveLift::vectorToMask(const SemanticOperand& vector,
                                      uint elementBits)
{
    return compareLanes(Op::COND_NOT_EQUAL, integerType(elementBits), vector,
                        zero(integerType(elementBits)));
}

SemanticOperand SveLift::laneIndex(int64 start, int64 step, uint elementBits)
{
    return emit(Op::OP_LANE_INDEX, integerType(elementBits),
                integer(start, 64), integer(step, 64));
}

SemanticOperand SveLift::laneIndexWide(int64 start, int64 step, uint lanes,
                                       uint elementBits)
{
    return emit(Op::OP_LANE_INDEX,
                SemanticType::vector(SemanticType::integer(elementBits), lanes),
                integer(start, 64), integer(step, 64));
}

SemanticOperand SveLift::emitFloat(SemanticOperation::Operation operation,
                                   const SemanticType& type,
                                   const SemanticOperand& a,
                                   const SemanticOperand& b,
                                   const SemanticOperand& c,
                                   SemanticOperation::Behavior behavior)
{
    SemanticOperand result = m_context.newTemporary(type);
    SemanticInstruction instruction(operation, type, result, a, b, c);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS | SemanticInstruction::ATTRIBUTE_FLOAT_IEEE_HALF);
    instruction.setBehavior(behavior);
    m_context.emit(instruction);
    return result;
}

SemanticOperand SveLift::integer(int64 value, uint bits)
{
    return m_context.getImmediate(value, SemanticType::integer(bits));
}

SemanticOperand SveLift::zero(const SemanticType& type)
{
    return m_context.getImmediate(0, type);
}

SemanticOperand SveLift::splat(const SemanticOperand& value,
                               const SemanticType& type)
{
    return emit(Op::OP_BROADCAST, type, value);
}

SemanticOperand SveLift::splatInteger(int64 value, uint elementBits)
{
    return splat(integer(value, elementBits), integerType(elementBits));
}

SemanticOperand SveLift::select(const SemanticOperand& mask,
                                const SemanticOperand& active,
                                const SemanticOperand& inactive)
{
    return emit(Op::OP_SELECT, active.getType(), mask, active, inactive);
}

SemanticOperand SveLift::bitcast(const SemanticOperand& value,
                                 const SemanticType& type)
{
    if (value.getType() == type)
        return value;
    return emit(Op::OP_BITCAST, type, value);
}

SemanticOperand SveLift::zeroExtend(const SemanticOperand& value, uint bits)
{
    if (value.getType().getTotalBits() >= bits)
        return value;
    return emit(Op::OP_ZERO_EXTEND, SemanticType::integer(bits), value);
}

SemanticOperand SveLift::truncate(const SemanticOperand& value, uint bits)
{
    if (value.getType().getTotalBits() <= bits)
        return value;
    return emit(Op::OP_TRUNCATE, SemanticType::integer(bits), value);
}

SemanticOperand SveLift::extractLane(const SemanticOperand& vector, uint lane)
{
    const SemanticType element = vector.getType().getElementType();
    return emit(Op::OP_EXTRACT, element, vector, integer(lane, 32));
}

SemanticOperand SveLift::insertLane(const SemanticOperand& vector,
                                    const SemanticOperand& value, uint lane)
{
    return emit(Op::OP_INSERT, vector.getType(), vector, value,
                integer(lane, 32));
}

SemanticOperand SveLift::readZWhole(uint number)
{
    // A copy: a register operand is read where it is used, and the
    // instruction may write the register before that
    SemanticOperand reg = m_context.getRegister(ARM64_REG_Z0 + number);
    if (reg.getType().getTotalBits() > m_vectorBits)
        return emit(Op::OP_TRUNCATE, wholeType(), reg);
    return emit(Op::OP_MOVE, reg.getType(), reg);
}

SemanticOperand SveLift::readZ(uint number, uint elementBits)
{
    return bitcast(readZWhole(number), integerType(elementBits));
}

void SveLift::writeZ(uint number, const SemanticOperand& value)
{
    SemanticOperand bits = value;
    if (bits.getType().getKind() != SemanticType::KIND_INTEGER ||
        bits.getType().isVector())
        bits = emit(Op::OP_BITCAST, wholeType(), bits);
    const SemanticOperand reg = m_context.getRegister(ARM64_REG_Z0 + number);
    if (reg.getType().getTotalBits() > bits.getType().getTotalBits())
        bits = emit(Op::OP_ZERO_EXTEND, reg.getType(), bits);
    m_context.move(reg, bits);
}

SemanticOperand SveLift::readPredicateBits(uint number)
{
    SemanticOperand reg = m_context.getRegister(ARM64_REG_P0 + number);
    if (reg.getType().getTotalBits() > predicateBits())
        return emit(Op::OP_TRUNCATE, predicateType(), reg);
    return emit(Op::OP_MOVE, reg.getType(), reg);
}

void SveLift::writePredicateBits(uint number, const SemanticOperand& value)
{
    SemanticOperand bits = value;
    if (bits.getType().getKind() != SemanticType::KIND_INTEGER ||
        bits.getType().isVector())
        bits = emit(Op::OP_BITCAST, SemanticType::integer(
                        bits.getType().getTotalBits()), bits);
    const SemanticOperand reg = m_context.getRegister(ARM64_REG_P0 + number);
    if (reg.getType().getTotalBits() > bits.getType().getTotalBits())
        bits = emit(Op::OP_ZERO_EXTEND, reg.getType(), bits);
    m_context.move(reg, bits);
}

SemanticOperand SveLift::readFfr()
{
    SemanticOperand reg = m_context.getRegister(ARM64_REG_FFR);
    if (reg.getType().getTotalBits() > predicateBits())
        return emit(Op::OP_TRUNCATE, predicateType(), reg);
    return emit(Op::OP_MOVE, reg.getType(), reg);
}

void SveLift::writeFfr(const SemanticOperand& value)
{
    SemanticOperand bits = value;
    const SemanticOperand reg = m_context.getRegister(ARM64_REG_FFR);
    if (reg.getType().getTotalBits() > bits.getType().getTotalBits())
        bits = emit(Op::OP_ZERO_EXTEND, reg.getType(), bits);
    m_context.move(reg, bits);
}

uint64 SveLift::elementBitsMask(uint elementBits) const
{
    uint64 mask = 0;
    const uint step = elementBits / 8;
    for (uint i = 0; i < predicateBits(); i += step)
        mask |= (uint64)1 << i;
    return mask;
}

SemanticOperand SveLift::elementPredicate(const SemanticOperand& predicate,
                                          uint elementBits)
{
    if (elementBits == 8)
        return predicate;
    return emit(Op::OP_AND, predicateType(), predicate,
                integer((int64)elementBitsMask(elementBits), predicateBits()));
}

SemanticOperand SveLift::readLaneMask(uint number, uint elementBits)
{
    SemanticOperand bits = readPredicateBits(number);
    const uint count = lanes(elementBits);
    if (elementBits == 8)
        return bitcast(bits, SemanticType::mask(count));
    SemanticOperand wide = zeroExtend(bits, 64);
    SemanticOperand packed = emit(Op::OP_BIT_GATHER, SemanticType::integer(64),
                                  wide, integer((int64)elementBitsMask(elementBits), 64));
    return bitcast(truncate(packed, count), SemanticType::mask(count));
}

SemanticOperand SveLift::maskToPredicate(const SemanticOperand& mask,
                                         uint elementBits)
{
    const uint count = lanes(elementBits);
    SemanticOperand bits = bitcast(mask, SemanticType::integer(count));
    if (elementBits == 8)
        return bits;
    SemanticOperand wide = zeroExtend(bits, 64);
    SemanticOperand spread = emit(Op::OP_BIT_DEPOSIT, SemanticType::integer(64),
                                  wide, integer((int64)elementBitsMask(elementBits), 64));
    return truncate(spread, predicateBits());
}

void SveLift::writeLaneMask(uint number, const SemanticOperand& mask,
                            uint elementBits)
{
    writePredicateBits(number, maskToPredicate(mask, elementBits));
}

SemanticOperand SveLift::laneMaskOfBits(const SemanticOperand& predicateBits,
                                        uint elementBits)
{
    const uint count = lanes(elementBits);
    if (elementBits == 8)
        return bitcast(predicateBits, SemanticType::mask(count));
    SemanticOperand wide = zeroExtend(predicateBits, 64);
    SemanticOperand packed = emit(Op::OP_BIT_GATHER, SemanticType::integer(64),
                                  wide, integer((int64)elementBitsMask(elementBits), 64));
    return bitcast(truncate(packed, count), SemanticType::mask(count));
}

SemanticOperand SveLift::counterPredicate(uint number, uint vectorIndex)
{
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand value = zeroExtend(truncate(readPredicateBits(number), 16), 64);
    SemanticOperand invert = emit(Op::OP_TRUNCATE, SemanticType::integer(1),
                                  emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, value, integer(15, 64)));
    uint predicateLog = 0;
    while ((1u << predicateLog) < predicateBits())
        predicateLog++;
    SemanticOperand result = integer(0, predicateBits());
    for (uint size = 0; size < 4; size++)
    {
        const uint counterBits = 8u << size;
        if (counterBits > 64)
            continue;
        const uint elements = lanes(counterBits);
        const SemanticType maskType = SemanticType::mask(elements);
        SemanticOperand marker = emit(Op::OP_AND, i64, value, integer((1 << (size + 1)) - 1, 64));
        SemanticOperand isValid = compareScalar(Op::COND_EQUAL, marker, integer(1 << size, 64));
        const int width = (int)predicateLog + 2 - (int)size;
        SemanticOperand count = emit(Op::OP_AND, i64,
            emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64, value, integer(size + 1, 64)),
            integer((int64)(((uint64)1 << (width > 0 ? width : 1)) - 1), 64));
        SemanticOperand below = emit(Op::OP_ACTIVE_LANE_MASK, maskType,
                                     integer(vectorIndex * elements, 64), count);
        SemanticOperand flip = bitcast(emit(Op::OP_SELECT, SemanticType::integer(elements),
                                            invert, integer(-1, elements), integer(0, elements)), maskType);
        SemanticOperand predicate = maskToPredicate(emit(Op::OP_XOR, maskType, below, flip), counterBits);
        result = emit(Op::OP_OR, predicateType(), result,
                      select(isValid, predicate, integer(0, predicateBits())));
    }
    return result;
}

SemanticOperand SveLift::counterActive(uint number, uint elementBits, uint vectors)
{
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand total = integer(0, 64);
    for (uint r = 0; r < vectors; r++)
    {
        SemanticOperand predicate = emit(Op::OP_AND, predicateType(), counterPredicate(number, r),
                                         integer((int64)elementBitsMask(elementBits), predicateBits()));
        total = emit(Op::OP_ADD, i64, total, emit(Op::OP_POPULATION_COUNT, i64, zeroExtend(predicate, 64)));
    }
    return total;
}

SemanticOperand SveLift::encodeCounter(const SemanticOperand& active, uint total,
                                       uint elementBits, bool isDescending)
{
    const SemanticType i64 = SemanticType::integer(64);
    uint size = 0;
    while ((8u << size) < elementBits)
        size++;
    const int64 marker = 1 << size;
    SemanticOperand all = integer(0x8000 | marker, 64);
    SemanticOperand count = isDescending ? emit(Op::OP_SUBTRACT, i64, integer(total, 64), active) : active;
    SemanticOperand partial = emit(Op::OP_OR, i64, emit(Op::OP_SHIFT_LEFT, i64, count, integer(size + 1, 64)),
                                   integer(marker | (isDescending ? 0x8000 : 0), 64));
    SemanticOperand result = select(compareScalar(Op::COND_EQUAL, active, integer(total, 64)), all, partial);
    return select(compareScalar(Op::COND_EQUAL, active, integer(0, 64)), integer(0, 64), result);
}

SemanticOperand SveLift::readZOperand(uint index)
{
    const ArmDecodedOperand& operand = m_context.getOperand(index);
    return readZ(registerNumber(operand), elementBitsOf(operand));
}

SemanticOperand SveLift::readZOperand(uint index, uint elementBits)
{
    return readZ(registerNumber(m_context.getOperand(index)), elementBits);
}

SemanticOperand SveLift::readMaskOperand(uint index, uint elementBits)
{
    return readLaneMask(registerNumber(m_context.getOperand(index)),
                        elementBits);
}

SemanticOperand SveLift::readZElement(uint index)
{
    const ArmDecodedOperand& operand = m_context.getOperand(index);
    const uint bits = elementBitsOf(operand);
    return extractLane(readZ(registerNumber(operand), bits),
                       (uint)operand.m_elementIndex);
}

SemanticOperand SveLift::readGprOperand(uint index, uint bits)
{
    const ArmDecodedOperand& operand = m_context.getOperand(index);
    const ArchitectureRegister& reg =
                    m_context.getArchitecture()->getRegister(operand.m_register);
    if (reg.m_isZero)
        return integer(0, bits);
    SemanticOperand value = m_context.getRegister(operand.m_register);
    if (value.getType().getTotalBits() > bits)
        return emit(Op::OP_TRUNCATE, SemanticType::integer(bits), value);
    if (value.getType().getTotalBits() < bits)
        return emit(Op::OP_ZERO_EXTEND, SemanticType::integer(bits), value);
    return value;
}

void SveLift::writeGprOperand(uint index, const SemanticOperand& value)
{
    const ArmDecodedOperand& operand = m_context.getOperand(index);
    const ArchitectureRegister& reg =
                    m_context.getArchitecture()->getRegister(operand.m_register);
    if (reg.m_isZero)
        return;
    SemanticOperand source = value;
    if (source.getType().getTotalBits() > reg.m_bits)
        source = emit(Op::OP_TRUNCATE, SemanticType::integer(reg.m_bits), source);
    m_context.move(m_context.getRegister(operand.m_register), source);
}

SemanticOperand SveLift::predicate(const SemanticOperand& mask,
                                   const SemanticOperand& computed,
                                   const SemanticOperand& old, bool isZeroing)
{
    if (isZeroing)
        return select(mask, computed, zero(computed.getType()));
    return select(mask, computed, old);
}

void SveLift::setPredicateFlags(const SemanticOperand& governing,
                                const SemanticOperand& result, uint elementBits)
{
    const SemanticType i64 = SemanticType::integer(64);
    SemanticOperand g = zeroExtend(elementPredicate(governing, elementBits), 64);
    SemanticOperand r = zeroExtend(result, 64);
    SemanticOperand none = compare(m_context, Op::COND_EQUAL, i64,
                                   emit(Op::OP_AND, i64, g, r), integer(0, 64));
    SemanticOperand negated = emit(Op::OP_NEGATE, i64, g);
    SemanticOperand lowest = emit(Op::OP_AND, i64, g, negated);
    SemanticOperand first = compare(m_context, Op::COND_NOT_EQUAL, i64,
                                    emit(Op::OP_AND, i64, r, lowest),
                                    integer(0, 64));
    SemanticOperand leading = emit(Op::OP_COUNT_LEADING_ZEROS, i64, g);
    SemanticOperand highest = emit(Op::OP_SHIFT_RIGHT_LOGICAL, i64,
                                   integer((int64)0x8000000000000000ULL, 64),
                                   leading);
    SemanticOperand notLast = compare(m_context, Op::COND_EQUAL, i64,
                                      emit(Op::OP_AND, i64, r, highest),
                                      integer(0, 64));
    m_context.setFlags(SemanticFlags::FLAG_SIGN, first);
    m_context.setFlags(SemanticFlags::FLAG_ZERO, none);
    m_context.setFlags(SemanticFlags::FLAG_CARRY, notLast);
    m_context.setFlags(SemanticFlags::FLAG_OVERFLOW, booleanConstant(m_context, false));
}

int64 SveLift::maximumSigned(uint bits)
{
    return (bits >= 64) ? (int64)0x7FFFFFFFFFFFFFFFLL
                        : (int64)(((uint64)1 << (bits - 1)) - 1);
}

int64 SveLift::minimumSigned(uint bits)
{
    return -maximumSigned(bits) - 1;
}

SemanticOperand SveLift::applyDefaultNan(const SemanticOperand& result,
                                         const SemanticType& type)
{
    // FPCR.DN (bit 25): every NaN result is the default NaN (positive, quiet,
    // no payload)
    const uint bits = type.getElementBits();
    const uint count = type.getLanes();
    const bool isVector = type.isVector();
    const SemanticType integerResult = isVector ? integerType(bits) : SemanticType::integer(bits);
    SemanticOperand control = m_context.getRegister(ARM64_REG_FPCR);
    SemanticOperand isDefault = emit(Op::OP_TRUNCATE, SemanticType::integer(1),
        emit(Op::OP_SHIFT_RIGHT_LOGICAL, control.getType(), control,
             integer(25, control.getType().getTotalBits())));
    uint64 pattern = 0x7E00;
    if (bits == 32)
        pattern = 0x7FC00000;
    else if (bits == 64)
        pattern = 0x7FF8000000000000ULL;
    else if (type.getFloatFormat() == SemanticType::FLOAT_BFLOAT16)
        pattern = 0x7FC0;
    SemanticOperand bitsResult = bitcast(result, integerResult);
    if (!isVector)
    {
        SemanticInstruction unordered(Op::OP_COMPARE, type, SemanticOperand(), result, result);
        SemanticOperand isNan = m_context.newTemporary(SemanticType::integer(1));
        unordered.setDestination(isNan);
        unordered.setCondition(Op::COND_FLOAT_UNORDERED);
        m_context.emit(unordered);
        SemanticOperand fix = emit(Op::OP_AND, SemanticType::integer(1), isNan, isDefault);
        return bitcast(emit(Op::OP_SELECT, integerResult, fix, integer((int64)pattern, bits),
                            bitsResult), type);
    }
    SemanticOperand isNan = compareLanes(Op::COND_FLOAT_UNORDERED, type, result, result);
    SemanticOperand defaultNan = splat(integer((int64)pattern, bits), integerResult);
    SemanticOperand enabled = emit(Op::OP_SELECT, SemanticType::integer(count),
                                   isDefault, integer(-1, count), integer(0, count));
    SemanticOperand fixMask = emit(Op::OP_AND, SemanticType::mask(count), isNan,
                                   bitcast(enabled, SemanticType::mask(count)));
    return bitcast(select(fixMask, defaultNan, bitsResult), type);
}

void SveLift::undefined()
{
    armdis::a64::undefined(m_context);
}

void armdis::a64::runSveTable(ArmLiftContext& context,
                              const SveDescriptor* table)
{
    SveLift lift(context);
    const char* name = getMnemonic(context);
    for (const SveDescriptor* i = table; i->m_name != NULL; i++)
    {
        if (strcmp(i->m_name, name) == 0)
        {
            i->m_handler(lift, *i);
            return;
        }
    }
    lift.undefined();
}
