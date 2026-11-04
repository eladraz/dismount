/*
 * ArmSemantics.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/smartptr.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticTemplate.h"
#include "dismount/semantics/OpcodeClassification.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

using namespace armdis;

typedef SemanticOperation Op;

namespace {

// The most temporaries and labels a template uses
enum { MAX_TEMPLATE_VALUES = 8 };

// Return the bit pattern of the floating point immediate 'value' in 'format'
uint64 getFloatBits(double value, SemanticType::FloatFormat format)
{
    if (format == SemanticType::FLOAT_DOUBLE)
    {
        union { double m_double; uint64 m_bits; } u;
        u.m_double = value;
        return u.m_bits;
    }
    // The single precision pattern: the half precision one is derived from it
    union { float m_float; uint32 m_bits; } s;
    s.m_float = (float)value;
    if (format == SemanticType::FLOAT_SINGLE)
        return s.m_bits;

    // binary16: the immediates of fmov are exactly representable (no
    // rounding, no subnormals), so rebias the exponent and cut the fraction
    const uint32 bits = s.m_bits;
    const uint32 sign = (bits >> 16) & 0x8000;
    const int exponent = (int)((bits >> 23) & 0xFF);
    if ((bits & 0x7FFFFFFF) == 0)
        return sign;
    return sign | (uint32)((exponent - 127 + 15) << 10) |
           ((bits >> 13) & 0x3FF);
}

// Return the element size of an arrangement in bits, 0 if the arrangement
// is not one of an element
uint getElementBits(uint arrangement)
{
    switch (arrangement)
    {
    case ArmDecodedOperand::ARRANGEMENT_8B:
    case ArmDecodedOperand::ARRANGEMENT_16B:
    case ArmDecodedOperand::ARRANGEMENT_B:
    case ArmDecodedOperand::ARRANGEMENT_4B:
        return 8;
    case ArmDecodedOperand::ARRANGEMENT_4H:
    case ArmDecodedOperand::ARRANGEMENT_8H:
    case ArmDecodedOperand::ARRANGEMENT_H:
    case ArmDecodedOperand::ARRANGEMENT_2H:
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
    default:
        return 0;
    }
}

// Return the total size of a vector arrangement in bits (8B is 64 bits), 0
// for the element arrangements
uint getVectorBits(uint arrangement)
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

// Return true and the register id if 'name' is the text of a fixed AArch32
// operand which is a register (the operands of the encodings which have the
// register implied: add r0, sp, #4; ldr r0, [sp]; push {lr}...)
bool getNamedRegister(const cString& name, uint& registerId)
{
    if (name == cString("sp"))
        registerId = ARM_REG_SP;
    else if (name == cString("lr"))
        registerId = ARM_REG_LR;
    else if (name == cString("pc"))
        registerId = ARM_REG_PC;
    else if (name == cString("r12"))
        registerId = ARM_REG_R12;
    else
        return false;
    return true;
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// ArmLiftContext
/////////////////////////////////////////////////////////////////////////////

ArmLiftContext::ArmLiftContext(const ArmOpcode& instruction,
                               SemanticBlock& block,
                               const ArchitectureDescriptionPtr& architecture) :
    m_instruction(instruction),
    m_block(block),
    m_architecture(architecture)
{
    for (uint i = 0; i < ArmOpcode::MAX_OPERANDS; i++)
        m_isAddressComputed[i] = false;
}

const ArmOpcode& ArmLiftContext::getInstruction() const
{
    return m_instruction;
}

SemanticBlock& ArmLiftContext::getBlock()
{
    return m_block;
}

const ArchitectureDescriptionPtr& ArmLiftContext::getArchitecture() const
{
    return m_architecture;
}

bool ArmLiftContext::isA64() const
{
    return m_instruction.getInstructionSet() == ArmInstructionSet::ARM_A64;
}

uint ArmLiftContext::getOperandsCount() const
{
    return m_instruction.getOperandsCount();
}

const ArmDecodedOperand& ArmLiftContext::getOperand(uint index) const
{
    return m_instruction.getOperand(index);
}

SemanticType ArmLiftContext::getNativeType() const
{
    return SemanticType::integer(isA64() ? 64 : 32);
}

SemanticType ArmLiftContext::getOperandType(uint index) const
{
    const ArmDecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case ArmDecodedOperand::KIND_REGISTER:
    case ArmDecodedOperand::KIND_SHIFTED_REGISTER:
    {
        const ArchitectureRegister& reg =
                                    m_architecture->getRegister(operand.m_register);
        // The vector arrangements are vectors of their element size
        const uint vectorBits = getVectorBits(operand.m_arrangement);
        const uint elementBits = getElementBits(operand.m_arrangement);
        if ((operand.m_elementIndex < 0) && (vectorBits != 0) &&
            (elementBits != 0))
            return SemanticType::vector(SemanticType::integer(elementBits),
                                        vectorBits / elementBits);
        if ((operand.m_elementIndex >= 0) && (elementBits != 0))
            return SemanticType::integer(elementBits);
        // An SVE z register: the lanes of its element size, scalable
        if ((reg.m_file == ArchitectureRegister::FILE_VECTOR) &&
            (elementBits != 0) && (operand.m_register >= ARM64_REG_Z0) &&
            (operand.m_register <= ARM64_REG_Z31) && isA64())
            return SemanticType::scalable(SemanticType::vector(
                        SemanticType::integer(elementBits),
                        128 / elementBits));
        if ((reg.m_file == ArchitectureRegister::FILE_MASK) && isA64())
            return SemanticType::scalable(SemanticType::mask(
                        (elementBits == 0) ? 16 : (128 / elementBits)));
        return SemanticType::integer(reg.m_bits);
    }
    case ArmDecodedOperand::KIND_MEMORY:
        // The access size is the one of the mnemonic: the lifter gives the
        // type explicitly. The register size is the default
        return getNativeType();
    case ArmDecodedOperand::KIND_FLOAT_IMMEDIATE:
        return SemanticType::floating(SemanticType::FLOAT_DOUBLE);
    default:
        return getNativeType();
    }
}

SemanticOperand ArmLiftContext::getRegister(uint registerId) const
{
    return SemanticOperand::registerOperand(registerId,
                    SemanticType::integer(
                        m_architecture->getRegister(registerId).m_bits));
}

SemanticOperand ArmLiftContext::getImmediate(int64 value,
                                             const SemanticType& type) const
{
    return SemanticOperand::immediate(value, type);
}

SemanticOperand ArmLiftContext::getCurrentAddress() const
{
    return SemanticOperand::guestAddress(m_instruction.getAddress(),
                                         getNativeType());
}

SemanticOperand ArmLiftContext::getNextAddress() const
{
    return SemanticOperand::guestAddress(
                    m_instruction.getAddress() + m_instruction.getOpcodeSize(),
                    getNativeType());
}

SemanticOperand ArmLiftContext::getPcValue() const
{
    // A32 reads the address + 8, T32 the address + 4
    const uint64 offset = (m_instruction.getInstructionSet() ==
                           ArmInstructionSet::ARM_T32) ? 4 : 8;
    return SemanticOperand::immediate(
                (int64)((m_instruction.getAddress() + offset) & 0xFFFFFFFFULL),
                SemanticType::integer(32));
}

SemanticOperation::Condition ArmLiftContext::convertCondition(uint condition)
{
    switch (condition & 15)
    {
    case 0:  return Op::COND_EQUAL;
    case 1:  return Op::COND_NOT_EQUAL;
    // The register C is the ARM carry
    case 2:  return Op::COND_CARRY;
    case 3:  return Op::COND_NOT_CARRY;
    case 4:  return Op::COND_NEGATIVE;
    case 5:  return Op::COND_NOT_NEGATIVE;
    case 6:  return Op::COND_OVERFLOW;
    case 7:  return Op::COND_NOT_OVERFLOW;
    // hi and ls combine C and Z
    case 8:
    case 9:  return Op::COND_NONE;
    case 10: return Op::COND_SIGNED_GREATER_EQUAL;
    case 11: return Op::COND_SIGNED_LESS;
    case 12: return Op::COND_SIGNED_GREATER;
    case 13: return Op::COND_SIGNED_LESS_EQUAL;
    default: return Op::COND_ALWAYS;
    }
}

SemanticOperation::Condition ArmLiftContext::getCondition() const
{
    return convertCondition(m_instruction.getCondition());
}

SemanticOperand ArmLiftContext::testCondition(uint condition, bool invert)
{
    condition &= 15;
    const SemanticType booleanType = SemanticType::integer(1);
    if (condition >= 14)
        return getImmediate(invert ? 0 : 1, booleanType);

    if ((condition == 8) || (condition == 9))
    {
        // hi = C and not Z, ls = not C or Z. The inverse of hi is ls
        const bool isHigher = (condition == 8) != invert;
        if (isHigher)
            return emit(Op::OP_AND, booleanType, testFlags(Op::COND_CARRY),
                        testFlags(Op::COND_NOT_EQUAL));
        return emit(Op::OP_OR, booleanType, testFlags(Op::COND_NOT_CARRY),
                    testFlags(Op::COND_EQUAL));
    }
    Op::Condition result = convertCondition(condition);
    if (invert)
        result = Op::invertCondition(result);
    return testFlags(result);
}

SemanticOperand ArmLiftContext::newTemporary(const SemanticType& type)
{
    return m_block.newTemporary(type);
}

SemanticOperand ArmLiftContext::newLabel()
{
    return m_block.newLabel();
}

SemanticOperand ArmLiftContext::emit(SemanticOperation::Operation operation,
                                     const SemanticType& type,
                                     const SemanticOperand& source0,
                                     const SemanticOperand& source1,
                                     const SemanticOperand& source2)
{
    SemanticOperand result = m_block.newTemporary(type);
    m_block.append(SemanticInstruction(operation, type, result,
                                       source0, source1, source2));
    return result;
}

void ArmLiftContext::emit(const SemanticInstruction& instruction)
{
    m_block.append(instruction);
}

void ArmLiftContext::move(const SemanticOperand& destination,
                          const SemanticOperand& source)
{
    m_block.append(SemanticInstruction(Op::OP_MOVE, destination.getType(),
                                       destination, source));
}

SemanticOperand ArmLiftContext::load(const SemanticOperand& address,
                                     const SemanticType& type)
{
    SemanticOperand result = m_block.newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    m_block.append(instruction);
    return result;
}

void ArmLiftContext::store(const SemanticOperand& address,
                           const SemanticOperand& value)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    m_block.append(instruction);
}

void ArmLiftContext::setFlags(SemanticOperation::FlagRecipe recipe,
                              uint32 flagMask,
                              const SemanticType& type,
                              const SemanticOperand& source0,
                              const SemanticOperand& source1,
                              const SemanticOperand& result)
{
    SemanticInstruction instruction(Op::OP_SET_FLAGS, type, SemanticOperand(),
                                    source0, source1, result);
    instruction.setFlags(recipe, flagMask);
    m_block.append(instruction);
}

void ArmLiftContext::setFlags(uint32 flagMask, const SemanticOperand& value)
{
    setFlags(Op::FLAGS_EXPLICIT, flagMask, SemanticType::integer(1),
             value, SemanticOperand(), SemanticOperand());
}

SemanticOperand ArmLiftContext::testFlags(SemanticOperation::Condition condition)
{
    const SemanticType booleanType = SemanticType::integer(1);
    SemanticOperand result = m_block.newTemporary(booleanType);
    SemanticInstruction instruction(Op::OP_TEST_FLAGS, booleanType, result);
    instruction.setCondition(condition);
    m_block.append(instruction);
    return result;
}

void ArmLiftContext::placeLabel(const SemanticOperand& label)
{
    m_block.append(SemanticInstruction(Op::OP_LABEL, SemanticType(),
                                       SemanticOperand(), label));
}

void ArmLiftContext::branch(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH, SemanticType(),
                                       SemanticOperand(), target));
}

void ArmLiftContext::branchIf(const SemanticOperand& condition,
                              const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH_CONDITIONAL,
                                       SemanticType(), SemanticOperand(),
                                       condition, target));
}

void ArmLiftContext::branchIndirect(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH_INDIRECT, SemanticType(),
                                       SemanticOperand(), target));
}

void ArmLiftContext::call(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_CALL, SemanticType(),
                                       SemanticOperand(), target));
}

void ArmLiftContext::returnTo(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_RETURN, SemanticType(),
                                       SemanticOperand(), target));
}

void ArmLiftContext::trap(SemanticOperation::TrapKind kind, uint number)
{
    SemanticInstruction instruction(Op::OP_TRAP, SemanticType(),
                                    SemanticOperand(),
                                    getImmediate(number, SemanticType::integer(32)));
    instruction.setKind(kind);
    m_block.append(instruction);
}

void ArmLiftContext::branchWritePc(const SemanticOperand& target,
                                   bool interwork)
{
    const SemanticType type = SemanticType::integer(32);
    SemanticOperand value = target;
    if (value.getType().getTotalBits() != 32)
        value = emit(Op::OP_TRUNCATE, type, value);

    if (!interwork)
    {
        // The Thumb pc is halfword aligned, the ARM one word aligned
        const bool isThumb = m_instruction.getInstructionSet() ==
                             ArmInstructionSet::ARM_T32;
        branchIndirect(emit(Op::OP_AND, type, value,
                            getImmediate(isThumb ? 0xFFFFFFFEu : 0xFFFFFFFCu,
                                         type)));
        return;
    }

    // BXWritePC: bit 0 selects T32. T32 clears bit 0, A32 bits 0 and 1
    uint modeRegister = ARM_REG_T;
    m_architecture->getModeRegister(modeRegister);
    const SemanticType booleanType = SemanticType::integer(1);
    SemanticOperand isThumb = emit(Op::OP_TRUNCATE, booleanType, value);
    move(getRegister(modeRegister), isThumb);
    SemanticOperand mask = emit(Op::OP_SELECT, type, isThumb,
                                getImmediate(0xFFFFFFFEu, type),
                                getImmediate(0xFFFFFFFCu, type));
    branchIndirect(emit(Op::OP_AND, type, value, mask));
}

SemanticOperand ArmLiftContext::read(uint index, const SemanticType& type)
{
    const ArmDecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case ArmDecodedOperand::KIND_REGISTER:
    case ArmDecodedOperand::KIND_SHIFTED_REGISTER:
    {
        SemanticOperand value;
        // AArch32 pc reads the address + 8 / + 4
        if (!isA64() && (operand.m_register == ARM_REG_PC))
            value = getPcValue();
        else
            value = getRegister(operand.m_register);

        // An element of a vector register
        if ((operand.m_kind == ArmDecodedOperand::KIND_REGISTER) &&
            (operand.m_elementIndex >= 0) &&
            (getElementBits(operand.m_arrangement) != 0))
        {
            const uint bits = getElementBits(operand.m_arrangement);
            return emit(Op::OP_EXTRACT, SemanticType::integer(bits), value,
                        getImmediate((int64)operand.m_elementIndex * bits,
                                     SemanticType::integer(32)));
        }
        if (operand.m_kind == ArmDecodedOperand::KIND_REGISTER)
        {
            // A vector arrangement: the low bits as a vector of lanes
            const uint vectorBits = getVectorBits(operand.m_arrangement);
            const uint elementBits = getElementBits(operand.m_arrangement);
            if ((vectorBits != 0) && (elementBits != 0))
            {
                if (vectorBits < value.getType().getTotalBits())
                    value = emit(Op::OP_TRUNCATE,
                                 SemanticType::integer(vectorBits), value);
                return emit(Op::OP_BITCAST,
                            SemanticType::vector(
                                SemanticType::integer(elementBits),
                                vectorBits / elementBits), value);
            }
            return value;
        }

        // The shift or the extend
        const uint bits = type.getTotalBits();
        const uint amount = operand.m_shiftAmount;
        const SemanticType shiftType = SemanticType::integer(bits);
        switch (operand.m_shift)
        {
        case ArmDecodedOperand::EXTEND_UXTB:
        case ArmDecodedOperand::EXTEND_UXTH:
        case ArmDecodedOperand::EXTEND_UXTW:
        case ArmDecodedOperand::EXTEND_UXTX:
        case ArmDecodedOperand::EXTEND_SXTB:
        case ArmDecodedOperand::EXTEND_SXTH:
        case ArmDecodedOperand::EXTEND_SXTW:
        case ArmDecodedOperand::EXTEND_SXTX:
        {
            const uint extend = operand.m_shift - ArmDecodedOperand::EXTEND_UXTB;
            const uint fromBits = 8u << (extend & 3);
            const bool isSigned = extend >= 4;
            if (fromBits < value.getType().getTotalBits())
                value = emit(Op::OP_TRUNCATE, SemanticType::integer(fromBits),
                             value);
            if (fromBits < bits)
                value = emit(isSigned ? Op::OP_SIGN_EXTEND : Op::OP_ZERO_EXTEND,
                             shiftType, value);
            if (amount != 0)
                value = emit(Op::OP_SHIFT_LEFT, shiftType, value,
                             getImmediate(amount, shiftType));
            return value;
        }
        case ArmDecodedOperand::SHIFT_LSL:
            if (amount != 0)
                value = emit(Op::OP_SHIFT_LEFT, shiftType, value,
                             getImmediate(amount, shiftType));
            return value;
        case ArmDecodedOperand::SHIFT_LSR:
        case ArmDecodedOperand::SHIFT_ASR:
        case ArmDecodedOperand::SHIFT_ROR:
        {
            const Op::Operation operation =
                (operand.m_shift == ArmDecodedOperand::SHIFT_LSR) ?
                    Op::OP_SHIFT_RIGHT_LOGICAL :
                (operand.m_shift == ArmDecodedOperand::SHIFT_ASR) ?
                    Op::OP_SHIFT_RIGHT_ARITHMETIC : Op::OP_ROTATE_RIGHT;
            // The A32 amounts of 32 (lsr / asr #32 are encoded as 0)
            if (amount != 0)
                value = emit(operation, shiftType, value,
                             getImmediate(amount, shiftType));
            return value;
        }
        case ArmDecodedOperand::SHIFT_RRX:
        {
            SemanticOperand carry = emit(Op::OP_ZERO_EXTEND, shiftType,
                                         getRegister(isA64() ? ARM64_REG_C :
                                                               ARM_REG_C));
            SemanticOperand high = emit(Op::OP_SHIFT_LEFT, shiftType, carry,
                                        getImmediate(bits - 1, shiftType));
            SemanticOperand low = emit(Op::OP_SHIFT_RIGHT_LOGICAL, shiftType,
                                       value, getImmediate(1, shiftType));
            return emit(Op::OP_OR, shiftType, high, low);
        }
        default:
            return value;
        }
    }
    case ArmDecodedOperand::KIND_IMMEDIATE:
    {
        int64 value = operand.m_immediate;
        if ((operand.m_shift == ArmDecodedOperand::SHIFT_LSL) &&
            (operand.m_shiftAmount < 64))
            value = (int64)((uint64)value << operand.m_shiftAmount);
        else if (operand.m_shift == ArmDecodedOperand::SHIFT_MSL)
            value = (int64)(((uint64)value << operand.m_shiftAmount) |
                            (((uint64)1 << operand.m_shiftAmount) - 1));
        return getImmediate(value, type);
    }
    case ArmDecodedOperand::KIND_FLOAT_IMMEDIATE:
        return getImmediate((int64)getFloatBits(operand.m_floatImmediate,
                                    type.getFloatFormat() ==
                                        SemanticType::FLOAT_NONE ?
                                    SemanticType::FLOAT_DOUBLE :
                                    type.getFloatFormat()), type);
    case ArmDecodedOperand::KIND_BRANCH_TARGET:
        return SemanticOperand::guestAddress(operand.m_immediate, type);
    case ArmDecodedOperand::KIND_CONDITION:
        return getImmediate(operand.m_condition, type);
    case ArmDecodedOperand::KIND_MEMORY:
        return load(getAddress(index), type);
    case ArmDecodedOperand::KIND_NAME:
    {
        // A fixed operand of AArch32: sp, lr, pc, r12 or the zero immediate
        uint registerId;
        if (getNamedRegister(operand.m_name, registerId))
        {
            if (registerId == ARM_REG_PC)
                return getPcValue();
            return getRegister(registerId);
        }
        CHECK((operand.m_name == cString("#0")) ||
              (operand.m_name == cString("#0x0")));
        return getImmediate(0, type);
    }
    default:
        // Lists, options and the matrix operands are read by the lifter of
        // the mnemonic
        CHECK_FAIL();
    }
}

SemanticOperand ArmLiftContext::read(uint index)
{
    return read(index, getOperandType(index));
}

void ArmLiftContext::write(uint index, const SemanticOperand& value)
{
    const ArmDecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case ArmDecodedOperand::KIND_REGISTER:
    {
        // AArch32: a write of pc is a branch
        if (!isA64() && (operand.m_register == ARM_REG_PC))
        {
            // The loads and the A32 data processing interwork
            const char* mnemonic = m_instruction.getEntry()->m_mnemonic;
            const bool isLoad = (strncmp(mnemonic, "ld", 2) == 0) ||
                                (strncmp(mnemonic, "pop", 3) == 0);
            branchWritePc(value, isLoad ||
                (m_instruction.getInstructionSet() == ArmInstructionSet::ARM_A32));
            return;
        }

        const ArchitectureRegister& reg =
                                m_architecture->getRegister(operand.m_register);
        SemanticOperand source = value;
        // An element of a vector register
        if ((operand.m_elementIndex >= 0) &&
            (getElementBits(operand.m_arrangement) != 0))
        {
            const uint bits = getElementBits(operand.m_arrangement);
            SemanticOperand lane = source;
            if (lane.getType().getTotalBits() != bits)
                lane = emit(Op::OP_TRUNCATE, SemanticType::integer(bits), lane);
            SemanticOperand updated = emit(Op::OP_INSERT, SemanticType::integer(
                                reg.m_bits), getRegister(operand.m_register),
                                lane, getImmediate((int64)operand.m_elementIndex *
                                    bits, SemanticType::integer(32)));
            move(getRegister(operand.m_register), updated);
            return;
        }
        // A vector arrangement is written as its bits: a 64 bit one zeroes
        // the upper half of the register
        if (source.getType().isVector())
            source = emit(Op::OP_BITCAST, SemanticType::integer(
                        source.getType().getTotalBits()), source);
        const uint bits = source.getType().getTotalBits();
        if (!source.getType().isScalable() && (bits < reg.m_bits) &&
            (reg.m_file != ArchitectureRegister::FILE_FLAG))
            source = emit(Op::OP_ZERO_EXTEND, SemanticType::integer(reg.m_bits),
                          source);
        else if (!source.getType().isScalable() && (bits > reg.m_bits))
            source = emit(Op::OP_TRUNCATE, SemanticType::integer(reg.m_bits),
                          source);
        if (!reg.m_isZero)
            move(getRegister(operand.m_register), source);
        break;
    }
    case ArmDecodedOperand::KIND_MEMORY:
        store(getAddress(index), value);
        break;
    case ArmDecodedOperand::KIND_NAME:
    {
        // A fixed register operand of AArch32 (sub sp, #4)
        uint registerId;
        CHECK(getNamedRegister(operand.m_name, registerId));
        if (registerId == ARM_REG_PC)
        {
            branchWritePc(value, m_instruction.getInstructionSet() ==
                                 ArmInstructionSet::ARM_A32);
            return;
        }
        move(getRegister(registerId), value);
        break;
    }
    default:
        // The immediates and the branch targets are not destinations
        CHECK_FAIL();
    }
}

SemanticOperand ArmLiftContext::getAddress(uint index)
{
    CHECK(index < ArmOpcode::MAX_OPERANDS);
    if (m_isAddressComputed[index])
        return m_addresses[index];

    const ArmDecodedOperand& operand = getOperand(index);
    CHECK(operand.m_kind == ArmDecodedOperand::KIND_MEMORY);
    const SemanticType type = getNativeType();

    SemanticOperand base;
    if (!isA64() && (operand.m_base == ARM_REG_PC))
        base = getImmediate(((m_instruction.getAddress() +
                    ((m_instruction.getInstructionSet() ==
                      ArmInstructionSet::ARM_T32) ? 4 : 8)) & ~(uint64)3),
                    type);
    else
        base = getRegister(operand.m_base);
    SemanticOperand address = base;

    // The post index forms access the base itself
    if (operand.m_addressing != ArmDecodedOperand::ADDRESSING_POST_INDEX)
    {
        if (operand.m_index != ARM_NO_REGISTER)
        {
            // The register offset: [base, index{, extend / shift #amount}]
            SemanticOperand offset = getRegister(operand.m_index);
            const uint extend = operand.m_shift;
            if (extend >= ArmDecodedOperand::EXTEND_UXTB)
            {
                const uint kind = extend - ArmDecodedOperand::EXTEND_UXTB;
                const uint fromBits = 8u << (kind & 3);
                if (fromBits < offset.getType().getTotalBits())
                    offset = emit(Op::OP_TRUNCATE,
                                  SemanticType::integer(fromBits), offset);
                if (fromBits < type.getTotalBits())
                    offset = emit(kind >= 4 ? Op::OP_SIGN_EXTEND :
                                              Op::OP_ZERO_EXTEND, type, offset);
            } else if (offset.getType().getTotalBits() < type.getTotalBits())
            {
                offset = emit(Op::OP_ZERO_EXTEND, type, offset);
            }
            if (operand.m_shiftAmount != 0)
            {
                const Op::Operation operation =
                    (operand.m_shift == ArmDecodedOperand::SHIFT_LSR) ?
                        Op::OP_SHIFT_RIGHT_LOGICAL :
                    (operand.m_shift == ArmDecodedOperand::SHIFT_ASR) ?
                        Op::OP_SHIFT_RIGHT_ARITHMETIC :
                    (operand.m_shift == ArmDecodedOperand::SHIFT_ROR) ?
                        Op::OP_ROTATE_RIGHT : Op::OP_SHIFT_LEFT;
                offset = emit(operation, type, offset,
                              getImmediate(operand.m_shiftAmount, type));
            }
            address = emit(operand.m_isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD,
                           type, base, offset);
        } else if (operand.m_offset != 0)
        {
            address = emit(Op::OP_ADD, type, base,
                           getImmediate(operand.m_isSubtract ?
                                        -operand.m_offset : operand.m_offset,
                                        type));
        }
    }
    // The instruction can change the base (ldp x0, x1, [x0]) before it
    // accesses all the memory: keep the value
    if (address.getKind() == SemanticOperand::KIND_REGISTER)
        address = emit(Op::OP_MOVE, address.getType(), address);
    m_addresses[index] = address;
    m_isAddressComputed[index] = true;
    return address;
}

void ArmLiftContext::writeBack(uint index)
{
    const ArmDecodedOperand& operand = getOperand(index);
    CHECK(operand.m_kind == ArmDecodedOperand::KIND_MEMORY);
    if (operand.m_addressing == ArmDecodedOperand::ADDRESSING_OFFSET)
        return;

    const SemanticType type = getNativeType();
    SemanticOperand address = getAddress(index);
    if (operand.m_addressing == ArmDecodedOperand::ADDRESSING_POST_INDEX)
    {
        // base += offset (or the post index register)
        if (operand.m_postIndexRegister != ARM_NO_REGISTER)
        {
            SemanticOperand offset = getRegister(operand.m_postIndexRegister);
            address = emit(operand.m_isSubtract ? Op::OP_SUBTRACT : Op::OP_ADD,
                           type, address, offset);
        } else if (operand.m_offset != 0)
        {
            address = emit(Op::OP_ADD, type, address,
                           getImmediate(operand.m_isSubtract ?
                                        -operand.m_offset : operand.m_offset,
                                        type));
        }
    }
    move(getRegister(operand.m_base), address);
}

/////////////////////////////////////////////////////////////////////////////
// The instantiation of the template steps
/////////////////////////////////////////////////////////////////////////////

namespace {

/*
 * The state of the instantiation of one template: its temporaries and labels
 * (created when they're first used).
 */
class TemplateInstance {
public:
    explicit TemplateInstance(ArmLiftContext& context) :
        m_context(context)
    {
    }

    // Instantiate all the steps
    void run(const SemanticTemplate& semanticTemplate)
    {
        for (uint i = 0; i < semanticTemplate.m_stepsCount; i++)
            step(semanticTemplate.m_steps[i]);
    }

private:
    // Return the type of a step
    SemanticType getType(const SemanticTemplateType& type) const
    {
        switch (type.m_source)
        {
        case SemanticTemplateType::SOURCE_FIXED:
            return SemanticType((SemanticType::Kind)type.m_kind, type.m_bits,
                                type.m_lanes);
        case SemanticTemplateType::SOURCE_OPERAND_SIZE:
        case SemanticTemplateType::SOURCE_ADDRESS_SIZE:
            return m_context.getNativeType();
        case SemanticTemplateType::SOURCE_OPERAND:
            return m_context.getOperandType(type.m_bits);
        case SemanticTemplateType::SOURCE_DOUBLE_OPERAND:
            return SemanticType::integer(
                    m_context.getOperandType(type.m_bits).getTotalBits() * 2);
        case SemanticTemplateType::SOURCE_BOOLEAN:
            return SemanticType::integer(1);
        default:
            // The vector types are given by the hand written lifters
            CHECK_FAIL();
        }
    }

    // Return the operand a reference reads
    SemanticOperand readReference(const SemanticTemplateReference& reference,
                                  const SemanticType& type)
    {
        switch (reference.m_kind)
        {
        case SemanticTemplateReference::KIND_NONE:
            return SemanticOperand();
        case SemanticTemplateReference::KIND_OPERAND:
            if (m_context.getOperand(reference.m_index).m_kind !=
                ArmDecodedOperand::KIND_MEMORY)
                return m_context.read(reference.m_index, type);
            // A memory operand is read once, and the write goes to the same
            // address
            if (!m_loaded[reference.m_index].isValid())
                m_loaded[reference.m_index] = m_context.read(reference.m_index,
                                                             type);
            return m_loaded[reference.m_index];
        case SemanticTemplateReference::KIND_OPERAND_ADDRESS:
            return m_context.getAddress(reference.m_index);
        case SemanticTemplateReference::KIND_TEMPORARY:
            CHECK((reference.m_index < MAX_TEMPLATE_VALUES) &&
                  m_temporaries[reference.m_index].isValid());
            return m_temporaries[reference.m_index];
        case SemanticTemplateReference::KIND_REGISTER:
            return m_context.getRegister(reference.m_value);
        case SemanticTemplateReference::KIND_IMMEDIATE:
            return m_context.getImmediate(reference.m_value, type);
        case SemanticTemplateReference::KIND_OPERATION_BYTES:
            return m_context.getImmediate(type.getTotalBits() / 8, type);
        case SemanticTemplateReference::KIND_NEXT_ADDRESS:
            return m_context.getNextAddress();
        case SemanticTemplateReference::KIND_CURRENT_ADDRESS:
            if (reference.m_value == 0)
                return m_context.getCurrentAddress();
            return m_context.getImmediate(
                (int64)(m_context.getInstruction().getAddress() +
                        (uint64)reference.m_value) & 0xFFFFFFFFLL,
                m_context.getNativeType());
        case SemanticTemplateReference::KIND_LABEL:
            CHECK(reference.m_index < MAX_TEMPLATE_VALUES);
            if (!m_labels[reference.m_index].isValid())
                m_labels[reference.m_index] = m_context.newLabel();
            return m_labels[reference.m_index];
        default:
            CHECK_FAIL();
        }
    }

    // Write the result of a step into its destination
    void writeReference(const SemanticTemplateReference& reference,
                        const SemanticOperand& value)
    {
        switch (reference.m_kind)
        {
        case SemanticTemplateReference::KIND_OPERAND:
            m_context.write(reference.m_index, value);
            m_loaded[reference.m_index] = SemanticOperand();
            break;
        case SemanticTemplateReference::KIND_TEMPORARY:
            CHECK(reference.m_index < MAX_TEMPLATE_VALUES);
            m_temporaries[reference.m_index] = value;
            break;
        case SemanticTemplateReference::KIND_REGISTER:
            m_context.move(m_context.getRegister(reference.m_value), value);
            break;
        default:
            CHECK_FAIL();
        }
    }

    // Instantiate a single step
    void step(const SemanticTemplateStep& step)
    {
        const SemanticType type = getType(step.m_type);
        const SemanticOperation::Operation operation =
                            (SemanticOperation::Operation)step.m_operation;

        SemanticOperand sources[SemanticInstruction::MAX_SOURCES];
        for (uint i = 0; i < SemanticInstruction::MAX_SOURCES; i++)
            sources[i] = readReference(step.m_sources[i], type);

        SemanticOperation::Condition condition =
                            (SemanticOperation::Condition)step.m_condition;
        if (step.m_condition == SemanticTemplate::CONDITION_FROM_OPCODE)
            condition = m_context.getCondition();

        switch (operation)
        {
        case SemanticOperation::OP_SET_FLAGS:
        {
            SemanticInstruction instruction(operation, type, SemanticOperand(),
                                            sources[0], sources[1], sources[2]);
            instruction.setFlags((SemanticOperation::FlagRecipe)step.m_flagRecipe,
                                 step.m_flagMask);
            instruction.setCondition(condition);
            m_context.emit(instruction);
            return;
        }
        case SemanticOperation::OP_STORE:
        case SemanticOperation::OP_LABEL:
        case SemanticOperation::OP_BRANCH:
        case SemanticOperation::OP_BRANCH_CONDITIONAL:
        case SemanticOperation::OP_BRANCH_INDIRECT:
        case SemanticOperation::OP_CALL:
        case SemanticOperation::OP_RETURN:
        case SemanticOperation::OP_NOP:
            // The operations without a value
            m_context.emit(SemanticInstruction(operation, SemanticType(),
                                               SemanticOperand(), sources[0],
                                               sources[1], sources[2]));
            return;
        default:
            break;
        }

        // A move straight into an operand needs no temporary
        if ((operation == SemanticOperation::OP_MOVE) &&
            (step.m_destination.m_kind == SemanticTemplateReference::KIND_OPERAND))
        {
            writeReference(step.m_destination, sources[0]);
            return;
        }

        // The result of a compare or a flags test is a boolean
        const bool isBoolean = (operation == SemanticOperation::OP_COMPARE) ||
                               (operation == SemanticOperation::OP_TEST_FLAGS);
        SemanticOperand result = m_context.newTemporary(
                        isBoolean ? SemanticType::integer(1) : type);
        SemanticInstruction instruction(operation, type, result,
                                        sources[0], sources[1], sources[2]);
        instruction.setCondition(condition);
        m_context.emit(instruction);
        writeReference(step.m_destination, result);
    }

    ArmLiftContext& m_context;
    SemanticOperand m_temporaries[MAX_TEMPLATE_VALUES];
    SemanticOperand m_labels[MAX_TEMPLATE_VALUES];
    SemanticOperand m_loaded[ArmOpcode::MAX_OPERANDS];
};

} // namespace

/////////////////////////////////////////////////////////////////////////////
// ArmSemanticLifter
/////////////////////////////////////////////////////////////////////////////

ArmSemanticLifter::ArmSemanticLifter(OpcodeSubsystems::DisassemblerType type,
                                     const ArchitectureDescriptionPtr& architecture) :
    m_type(type),
    m_architecture(architecture)
{
    CHECK((type == OpcodeSubsystems::DISASSEMBLER_ARM_32_LE) ||
          (type == OpcodeSubsystems::DISASSEMBLER_THUMB_LE) ||
          (type == OpcodeSubsystems::DISASSEMBLER_ARM_64_LE));
    if (m_architecture.getPointer() == NULL)
        m_architecture = ArchitectureDescriptionFactory::getArchitecture(type);
}

ArchitectureDescriptionPtr ArmSemanticLifter::getArchitecture() const
{
    return m_architecture;
}

namespace {

// An encoding table and the semantic table of its area
struct AreaTable {
    const armdis::ArmEncodingEntry* m_encoding;
    const armdis::ArmSemanticEntry* m_semantic;
    // The end of the encoding table, found on the first search
    mutable const armdis::ArmEncodingEntry* m_end;
};

// Return the semantic table of the area whose encoding table has 'entry'
// (the mnemonics of two areas are the same: the vector "add" is not the
// scalar one), or NULL if the entry belongs to none of 'areas'
const armdis::ArmSemanticEntry* findArea(const AreaTable* areas, uint count,
                                         const armdis::ArmEncodingEntry* entry)
{
    for (uint i = 0; i < count; i++)
    {
        if (areas[i].m_end == NULL)
        {
            const armdis::ArmEncodingEntry* end = areas[i].m_encoding;
            while (end->m_mnemonic != NULL)
                end++;
            areas[i].m_end = end;
        }
        if ((entry >= areas[i].m_encoding) && (entry < areas[i].m_end))
            return areas[i].m_semantic;
    }
    return NULL;
}

// Search the table of an area for a mnemonic
const armdis::ArmSemanticEntry* searchTable(const armdis::ArmSemanticEntry* table,
                                            const char* name,
                                            uint operandsCount)
{
    // The T32 wide encodings are named "add.w", "ldr.w"...: the semantics are
    // those of the mnemonic without the suffix
    size_t length = strlen(name);
    if ((length > 2) && (name[length - 2] == '.') && (name[length - 1] == 'w'))
        length -= 2;
    for (const armdis::ArmSemanticEntry* entry = table;
         entry->m_template.m_name != NULL; entry++)
    {
        if ((entry->m_operandsCount != armdis::ArmSemanticEntry::ANY_OPERANDS_COUNT) &&
            (entry->m_operandsCount != operandsCount))
            continue;
        if ((strlen(entry->m_template.m_name) == length) &&
            (strncmp(entry->m_template.m_name, name, length) == 0))
            return entry;
    }
    return NULL;
}

} // namespace

const armdis::ArmSemanticEntry* ArmSemanticLifter::findEntry(
                                            const ArmOpcode& instruction)
{
    const armdis::ArmEncodingEntry* encoding = instruction.getEntry();
    const char* name = encoding->m_mnemonic;
    const uint operandsCount = instruction.getOperandsCount();

    static const armdis::ArmSemanticEntry* const a64Tables[] = {
        gArm64SemanticTable, gArm64SimdSemanticTable,
        gArm64SveSemanticTable, gArm64SmeSemanticTable
    };
    static const armdis::ArmSemanticEntry* const a32Tables[] = {
        gArmSemanticTable, gArmSimdSemanticTable
    };
    // The area of the encoding entry decides the semantic table. An entry
    // which isn't in the tables (a hand built instruction) searches them all
    static const AreaTable a64Areas[] = {
        { armdis::gArm64BaseEncodingTable, armdis::gArm64SemanticTable, NULL },
        { armdis::gArm64SimdEncodingTable, armdis::gArm64SimdSemanticTable, NULL },
        { armdis::gArm64SveEncodingTable, armdis::gArm64SveSemanticTable, NULL },
        { armdis::gArm64SmeEncodingTable, armdis::gArm64SmeSemanticTable, NULL }
    };
    static const AreaTable a32Areas[] = {
        { armdis::gArmA32EncodingTable, armdis::gArmSemanticTable, NULL },
        { armdis::gArmT16EncodingTable, armdis::gArmSemanticTable, NULL },
        { armdis::gArmT32EncodingTable, armdis::gArmSemanticTable, NULL },
        { armdis::gArmA32SimdEncodingTable, armdis::gArmSimdSemanticTable, NULL }
    };
    const bool isA64 = instruction.getInstructionSet() ==
                       ArmInstructionSet::ARM_A64;
    const armdis::ArmSemanticEntry* area = isA64 ?
        findArea(a64Areas, sizeof(a64Areas) / sizeof(a64Areas[0]), encoding) :
        findArea(a32Areas, sizeof(a32Areas) / sizeof(a32Areas[0]), encoding);
    if (area != NULL)
    {
        const armdis::ArmSemanticEntry* found = searchTable(area, name, operandsCount);
        // The SME vector length instructions (rdsvl, addsvl, addspl) are in the SVE encoding
        // table: when the SVE table has no entry for them, the SME one does
        if ((found == NULL) && (area == armdis::gArm64SveSemanticTable) &&
            (encoding->m_feature == ArmFeatures::FEAT_SME))
            found = searchTable(armdis::gArm64SmeSemanticTable, name, operandsCount);
        return found;
    }

    const armdis::ArmSemanticEntry* const* tables = isA64 ? a64Tables : a32Tables;
    const uint tablesCount = isA64 ?
        (uint)(sizeof(a64Tables) / sizeof(a64Tables[0])) :
        (uint)(sizeof(a32Tables) / sizeof(a32Tables[0]));
    for (uint i = 0; i < tablesCount; i++)
    {
        const armdis::ArmSemanticEntry* found = searchTable(tables[i], name,
                                                            operandsCount);
        if (found != NULL)
            return found;
    }
    return NULL;
}

OpcodeClassification ArmSemanticLifter::classify(const OpcodePtr& instruction) const
{
    if (instruction->getType() != m_type)
        return OpcodeClassification();
    const ArmOpcode& arm = *(const ArmOpcode*)instruction.getPointer();
    const armdis::ArmSemanticEntry* entry = findEntry(arm);
    if (entry == NULL)
        return OpcodeClassification();

    const SemanticTemplate& semanticTemplate = entry->m_template;
    const uint count = arm.getOperandsCount();
    uint32 properties = semanticTemplate.m_properties;
    uint32 units = semanticTemplate.m_units;

    // The memory the operands access
    bool hasMemoryOperand = false;
    for (uint i = 0; i < count; i++)
        if (arm.getOperand(i).m_kind == ArmDecodedOperand::KIND_MEMORY)
            hasMemoryOperand = true;
    const bool isImplicitMemory =
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_STACK) ||
        (semanticTemplate.m_category == OpcodeClassification::CATEGORY_ATOMIC) ||
        ((properties & OpcodeClassification::PROPERTY_EXCLUSIVE) != 0);
    if (!hasMemoryOperand && !isImplicitMemory)
    {
        properties &= ~(OpcodeClassification::PROPERTY_READS_MEMORY |
                        OpcodeClassification::PROPERTY_WRITES_MEMORY |
                        OpcodeClassification::PROPERTY_MAY_FAULT);
        units &= ~(OpcodeClassification::UNIT_LOAD |
                   OpcodeClassification::UNIT_STORE);
    } else
    {
        properties |= OpcodeClassification::PROPERTY_MAY_FAULT;
    }

    // AArch32 data processing: the table has the flags of the S form, the
    // instruction writes them only with the S bit (the compares always do)
    uint32 flagsWritten = semanticTemplate.m_flagsWritten;
    if ((arm.getInstructionSet() != ArmInstructionSet::ARM_A64) &&
        !arm.isSettingFlags() &&
        ((semanticTemplate.m_category == OpcodeClassification::CATEGORY_ARITHMETIC) ||
         (semanticTemplate.m_category == OpcodeClassification::CATEGORY_LOGIC) ||
         (semanticTemplate.m_category == OpcodeClassification::CATEGORY_MOVE) ||
         (semanticTemplate.m_category == OpcodeClassification::CATEGORY_SHIFT) ||
         (semanticTemplate.m_category == OpcodeClassification::CATEGORY_MULTIPLY)))
        flagsWritten = 0;

    // The flags properties follow the flags sets
    if (semanticTemplate.m_flagsRead != 0)
        properties |= OpcodeClassification::PROPERTY_READS_FLAGS;
    if (flagsWritten != 0)
        properties |= OpcodeClassification::PROPERTY_WRITES_FLAGS;
    if (semanticTemplate.m_flagsUndefined != 0)
        properties |= OpcodeClassification::PROPERTY_UNDEFINED_FLAGS;
    // The instruction is conditional (AArch32 cond, T32 it block)
    if ((arm.getInstructionSet() != ArmInstructionSet::ARM_A64) &&
        (arm.getCondition() != ArmOpcode::CONDITION_ALWAYS))
        properties |= OpcodeClassification::PROPERTY_CONDITIONAL |
                      OpcodeClassification::PROPERTY_READS_FLAGS;

    OpcodeClassification result(
                (OpcodeClassification::Category)semanticTemplate.m_category,
                units, properties);
    result.setFlags(semanticTemplate.m_flagsRead, flagsWritten,
                    semanticTemplate.m_flagsUndefined);

    // The size of the data: the entry gives the vector and the floating point
    // element, otherwise the first register operand
    if (entry->m_dataKind != SemanticType::KIND_VOID)
    {
        uint lanes = entry->m_dataLanes;
        if (lanes == 0)
        {
            uint totalBits = 128;
            if ((count > 0) &&
                (arm.getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER))
                totalBits = m_architecture->getRegister(
                                    arm.getOperand(0).m_register).m_bits;
            if (getVectorBits(arm.getOperand(0).m_arrangement) != 0)
                totalBits = getVectorBits(arm.getOperand(0).m_arrangement);
            lanes = totalBits / entry->m_dataBits;
            if (lanes == 0)
                lanes = 1;
        }
        result.setDataType(SemanticType(
                    (SemanticType::Kind)entry->m_dataKind, entry->m_dataBits,
                    lanes, (SemanticType::FloatFormat)entry->m_dataFormat));
    } else if ((count > 0) &&
               ((arm.getOperand(0).m_kind == ArmDecodedOperand::KIND_REGISTER) ||
                (arm.getOperand(0).m_kind ==
                 ArmDecodedOperand::KIND_SHIFTED_REGISTER)))
    {
        result.setDataType(SemanticType::integer(
                    m_architecture->getRegister(
                                arm.getOperand(0).m_register).m_bits));
    } else
    {
        result.setDataType(SemanticType::integer(
                    (arm.getInstructionSet() == ArmInstructionSet::ARM_A64) ?
                        64 : 32));
    }
    return result;
}

bool ArmSemanticLifter::lift(const OpcodePtr& instruction,
                             SemanticBlock& block) const
{
    CHECK(block.getArchitecture() == m_type);

    ProcessorAddress address(gNullPointerProcessorAddress);
    instruction->getOpcodeAddress(address);
    block.beginGuestInstruction(address, instruction->getOpcodeSize());

    // An invalid instruction has no semantics
    const armdis::ArmSemanticEntry* entry = NULL;
    const ArmOpcode* arm = NULL;
    if (instruction->getType() == m_type)
    {
        arm = (const ArmOpcode*)instruction.getPointer();
        // The UNPREDICTABLE encodings are lifted to OP_UNDEFINED
        if ((arm->getEntry()->m_flags & ARM_ENTRY_UNPREDICTABLE) == 0)
            entry = findEntry(*arm);
    }
    if (entry == NULL)
    {
        block.append(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                                         SemanticOperand()));
        return false;
    }

    ArmLiftContext context(*arm, block, m_architecture);

    // AArch32: the condition (the field of A32, the it block of T32, or the
    // condition of the T16 / T32 branches) wraps the instruction: a branch
    // over it when the condition fails. A64 has no conditional execution:
    // the conditional branches and selects test it themselves
    SemanticOperand skip;
    if (!context.isA64() && (arm->getCondition() != ArmOpcode::CONDITION_ALWAYS))
    {
        skip = context.newLabel();
        context.branchIf(context.testCondition(arm->getCondition(), true), skip);
    }

    if (entry->m_lift != NULL)
        entry->m_lift(context);
    else
        instantiate(entry->m_template, context);

    if (skip.isValid())
        context.placeLabel(skip);
    return true;
}

void ArmSemanticLifter::instantiate(const SemanticTemplate& semanticTemplate,
                                    ArmLiftContext& context) const
{
    TemplateInstance instance(context);
    instance.run(semanticTemplate);
}
