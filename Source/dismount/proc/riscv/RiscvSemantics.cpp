/*
 * RiscvSemantics.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "dismount/dismount.h"
#include <string.h>

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
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvArchitecture.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"
#include "dismount/proc/riscv/RiscvSemantics.h"

using namespace riscvdis;

typedef SemanticOperation Op;

namespace {

// Return the mask of the low 'bits' bits
uint64 lowMask(uint bits)
{
    return (bits >= 64) ? ~(uint64)0 : (((uint64)1 << bits) - 1);
}

// Return true if the register id is a floating point register
bool isFloatRegister(uint registerId)
{
    return (registerId >= RISCV_REG_F0) && (registerId <= RISCV_REG_F31);
}

// Return the canonical NaN of a floating point format of the boxed
// registers, the pattern a badly boxed value reads as
uint64 getCanonicalNan(const SemanticType& type)
{
    switch (type.getFloatFormat())
    {
    case SemanticType::FLOAT_HALF:
        return 0x7E00;
    case SemanticType::FLOAT_BFLOAT16:
        return 0x7FC0;
    case SemanticType::FLOAT_SINGLE:
        return 0x7FC00000ULL;
    default:
        CHECK_FAIL();
    }
}

} // namespace

/////////////////////////////////////////////////////////////////////////////
// RiscvLiftContext
/////////////////////////////////////////////////////////////////////////////

RiscvLiftContext::RiscvLiftContext(const RiscvOpcode& instruction,
                                   SemanticBlock& block,
                                   const ArchitectureDescriptionPtr& architecture) :
    m_instruction(instruction),
    m_resolved(NULL),
    m_block(block),
    m_architecture(architecture)
{
    m_resolved = RiscvSemanticLifter::resolve(instruction, m_resolvedStorage);
    CHECK(m_resolved != NULL);
    for (uint i = 0; i < RiscvOpcode::MAX_OPERANDS; i++)
        m_isAddressComputed[i] = false;
}

const RiscvOpcode& RiscvLiftContext::getInstruction() const
{
    return *m_resolved;
}

uint RiscvLiftContext::getSize() const
{
    return m_instruction.getOpcodeSize();
}

SemanticBlock& RiscvLiftContext::getBlock()
{
    return m_block;
}

const ArchitectureDescriptionPtr& RiscvLiftContext::getArchitecture() const
{
    return m_architecture;
}

uint RiscvLiftContext::getXlen() const
{
    return m_architecture->getNativeIntegerBits();
}

uint RiscvLiftContext::getVectorBits() const
{
    return m_architecture->getMaximumVectorBits();
}

uint RiscvLiftContext::getOperandsCount() const
{
    return m_resolved->getOperandsCount();
}

const RiscvDecodedOperand& RiscvLiftContext::getOperand(uint index) const
{
    return m_resolved->getOperand(index);
}

SemanticType RiscvLiftContext::getNativeType() const
{
    return SemanticType::integer(getXlen());
}

SemanticOperand RiscvLiftContext::getRegister(uint registerId) const
{
    return SemanticOperand::registerOperand(registerId,
                    SemanticType::integer(
                        m_architecture->getRegister(registerId).m_bits));
}

SemanticOperand RiscvLiftContext::getImmediate(int64 value,
                                               const SemanticType& type) const
{
    return SemanticOperand::immediate(value, type);
}

SemanticOperand RiscvLiftContext::getCurrentAddress() const
{
    return SemanticOperand::guestAddress(
                    m_instruction.getAddress() & lowMask(getXlen()),
                    getNativeType());
}

SemanticOperand RiscvLiftContext::getNextAddress() const
{
    return SemanticOperand::guestAddress(
                    (m_instruction.getAddress() + getSize()) &
                        lowMask(getXlen()),
                    getNativeType());
}

uint RiscvLiftContext::getRoundingMode(uint index) const
{
    // The default of an instruction without the operand is the dynamic mode
    uint mode = 7;
    if ((index < getOperandsCount()) &&
        (getOperand(index).m_kind == RiscvDecodedOperand::KIND_ROUNDING_MODE))
        mode = getOperand(index).m_roundingMode;

    switch (mode)
    {
    case 0: return Op::ROUND_NEAREST_EVEN;
    case 1: return Op::ROUND_TOWARD_ZERO;
    case 2: return Op::ROUND_TOWARD_NEGATIVE;
    case 3: return Op::ROUND_TOWARD_POSITIVE;
    case 4: return Op::ROUND_NEAREST_AWAY;
    case 7:
    {
        // The dynamic mode: frm, where 5 and 6 (and 7) are reserved and make
        // the instruction an illegal one
        const SemanticType frmType = SemanticType::integer(3);
        const SemanticType booleanType = SemanticType::integer(1);
        SemanticOperand ok = m_block.newTemporary(booleanType);
        SemanticInstruction compare(Op::OP_COMPARE, frmType, ok,
                                    getRegister(RISCV_REG_FRM),
                                    getImmediate(4, frmType));
        compare.setCondition(Op::COND_UNSIGNED_LESS_EQUAL);
        m_block.append(compare);
        SemanticOperand skip = m_block.newLabel();
        m_block.append(SemanticInstruction(Op::OP_BRANCH_CONDITIONAL,
                                           SemanticType(), SemanticOperand(),
                                           ok, skip));
        SemanticInstruction trapInstruction(Op::OP_TRAP, SemanticType(),
                SemanticOperand(),
                getImmediate(RISCV_TRAP_ILLEGAL_INSTRUCTION,
                             SemanticType::integer(32)));
        trapInstruction.setKind(Op::TRAP_UNDEFINED);
        m_block.append(trapInstruction);
        m_block.append(SemanticInstruction(Op::OP_LABEL, SemanticType(),
                                           SemanticOperand(), skip));
        return Op::ROUND_DYNAMIC;
    }
    default:
        // The reserved values: an illegal instruction
        SemanticInstruction trapInstruction(Op::OP_TRAP, SemanticType(),
                SemanticOperand(),
                getImmediate(RISCV_TRAP_ILLEGAL_INSTRUCTION,
                             SemanticType::integer(32)));
        trapInstruction.setKind(Op::TRAP_UNDEFINED);
        m_block.append(trapInstruction);
        return Op::ROUND_NEAREST_EVEN;
    }
}

SemanticOperand RiscvLiftContext::newTemporary(const SemanticType& type)
{
    return m_block.newTemporary(type);
}

SemanticOperand RiscvLiftContext::newLabel()
{
    return m_block.newLabel();
}

SemanticOperand RiscvLiftContext::emit(SemanticOperation::Operation operation,
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

void RiscvLiftContext::emit(const SemanticInstruction& instruction)
{
    m_block.append(instruction);
}

void RiscvLiftContext::move(const SemanticOperand& destination,
                            const SemanticOperand& source)
{
    m_block.append(SemanticInstruction(Op::OP_MOVE, destination.getType(),
                                       destination, source));
}

SemanticOperand RiscvLiftContext::load(const SemanticOperand& address,
                                       const SemanticType& type)
{
    SemanticOperand result = m_block.newTemporary(type);
    SemanticInstruction instruction(Op::OP_LOAD, type, result, address);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    m_block.append(instruction);
    return result;
}

void RiscvLiftContext::store(const SemanticOperand& address,
                             const SemanticOperand& value)
{
    SemanticInstruction instruction(Op::OP_STORE, value.getType(),
                                    SemanticOperand(), address, value);
    instruction.setAttributes(SemanticInstruction::ATTRIBUTE_MAY_FAULT);
    m_block.append(instruction);
}

void RiscvLiftContext::placeLabel(const SemanticOperand& label)
{
    m_block.append(SemanticInstruction(Op::OP_LABEL, SemanticType(),
                                       SemanticOperand(), label));
}

void RiscvLiftContext::branch(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH, SemanticType(),
                                       SemanticOperand(), target));
}

void RiscvLiftContext::branchIf(const SemanticOperand& condition,
                                const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH_CONDITIONAL,
                                       SemanticType(), SemanticOperand(),
                                       condition, target));
}

void RiscvLiftContext::trap(SemanticOperation::TrapKind kind, uint number)
{
    SemanticInstruction instruction(Op::OP_TRAP, SemanticType(),
                                    SemanticOperand(),
                                    getImmediate(number, SemanticType::integer(32)));
    instruction.setKind(kind);
    m_block.append(instruction);
}

void RiscvLiftContext::branchIndirect(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_BRANCH_INDIRECT, SemanticType(),
                                       SemanticOperand(), target));
}

void RiscvLiftContext::call(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_CALL, SemanticType(),
                                       SemanticOperand(), target));
}

void RiscvLiftContext::returnTo(const SemanticOperand& target)
{
    m_block.append(SemanticInstruction(Op::OP_RETURN, SemanticType(),
                                       SemanticOperand(), target));
}

SemanticOperand RiscvLiftContext::read(uint index, const SemanticType& type)
{
    const RiscvDecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case RiscvDecodedOperand::KIND_REGISTER:
    {
        // A floating point value of a floating point register is unboxed
        if (isFloatRegister(operand.m_register) &&
            (type.getKind() == SemanticType::KIND_FLOAT))
            return readFloat(index, type);

        SemanticOperand value;
        if (operand.m_register == RISCV_REG_X0)
            value = getImmediate(0, getNativeType());
        else
            value = getRegister(operand.m_register);
        // The integer registers are read as the size of the type
        const uint bits = type.getTotalBits();
        const uint registerBits = value.getType().getTotalBits();
        if (type.isScalable() || type.isVector() ||
            (type.getKind() != SemanticType::KIND_INTEGER) ||
            (bits == registerBits))
            return value;
        if (bits < registerBits)
            return emit(Op::OP_TRUNCATE, type, value);
        return emit(Op::OP_ZERO_EXTEND, type, value);
    }
    case RiscvDecodedOperand::KIND_IMMEDIATE:
        return getImmediate(operand.m_immediate, type);
    case RiscvDecodedOperand::KIND_FLOAT_IMMEDIATE:
    {
        CHECK(operand.m_fliIndex < 32);
        const RiscvFliValue& value = gRiscvFliValues[operand.m_fliIndex];
        switch (type.getFloatFormat())
        {
        case SemanticType::FLOAT_HALF:
            return getImmediate((int64)value.m_half, type);
        case SemanticType::FLOAT_SINGLE:
            return getImmediate((int64)value.m_single, type);
        default:
            return getImmediate((int64)value.m_double, type);
        }
    }
    case RiscvDecodedOperand::KIND_BRANCH_TARGET:
        return SemanticOperand::guestAddress(
                    (uint64)operand.m_immediate & lowMask(getXlen()), type);
    case RiscvDecodedOperand::KIND_MEMORY:
        return load(getAddress(index), type);
    case RiscvDecodedOperand::KIND_CSR:
        return getImmediate(operand.m_csr, type);
    case RiscvDecodedOperand::KIND_ROUNDING_MODE:
        return getImmediate(operand.m_roundingMode, type);
    case RiscvDecodedOperand::KIND_FENCE_SET:
        return getImmediate(operand.m_fenceSet, type);
    case RiscvDecodedOperand::KIND_VTYPE:
        return getImmediate(operand.m_vtype, type);
    case RiscvDecodedOperand::KIND_VECTOR_MASK:
        return getRegister(RISCV_REG_V0);
    default:
        CHECK_FAIL();
    }
}

SemanticOperand RiscvLiftContext::read(uint index)
{
    const RiscvDecodedOperand& operand = getOperand(index);
    if (operand.m_kind == RiscvDecodedOperand::KIND_REGISTER)
    {
        if (operand.m_register == RISCV_REG_X0)
            return getImmediate(0, getNativeType());
        return getRegister(operand.m_register);
    }
    return read(index, getNativeType());
}

void RiscvLiftContext::write(uint index, const SemanticOperand& value)
{
    const RiscvDecodedOperand& operand = getOperand(index);
    switch (operand.m_kind)
    {
    case RiscvDecodedOperand::KIND_REGISTER:
    {
        // The writes of x0 are dropped
        if (operand.m_register == RISCV_REG_X0)
            return;
        // A floating point value is boxed
        if (isFloatRegister(operand.m_register) &&
            (value.getType().getKind() == SemanticType::KIND_FLOAT))
        {
            writeFloat(index, value);
            return;
        }
        const ArchitectureRegister& reg =
                                m_architecture->getRegister(operand.m_register);
        SemanticOperand source = value;
        if (source.getType().isVector() || (source.getType().getKind() !=
                                            SemanticType::KIND_INTEGER))
            source = emit(Op::OP_BITCAST, SemanticType::integer(
                        source.getType().getTotalBits()), source);
        const uint bits = source.getType().getTotalBits();
        if (!source.getType().isScalable() && (bits < reg.m_bits))
            source = emit(Op::OP_ZERO_EXTEND, SemanticType::integer(reg.m_bits),
                          source);
        else if (!source.getType().isScalable() && (bits > reg.m_bits))
            source = emit(Op::OP_TRUNCATE, SemanticType::integer(reg.m_bits),
                          source);
        move(getRegister(operand.m_register), source);
        break;
    }
    case RiscvDecodedOperand::KIND_MEMORY:
        store(getAddress(index), value);
        break;
    default:
        // The immediates and the branch targets are not destinations
        CHECK_FAIL();
    }
}

void RiscvLiftContext::writeSignExtended(uint index, const SemanticOperand& value)
{
    SemanticOperand source = value;
    if (source.getType().getTotalBits() < getXlen())
        source = emit(Op::OP_SIGN_EXTEND, getNativeType(), source);
    write(index, source);
}

SemanticOperand RiscvLiftContext::readFloat(uint index, const SemanticType& type)
{
    const RiscvDecodedOperand& operand = getOperand(index);
    CHECK(operand.m_kind == RiscvDecodedOperand::KIND_REGISTER);
    CHECK(isFloatRegister(operand.m_register));
    CHECK(type.getKind() == SemanticType::KIND_FLOAT);
    const uint bits = type.getTotalBits();
    const SemanticType registerType = SemanticType::integer(64);
    SemanticOperand value = getRegister(operand.m_register);
    if (bits == 64)
        return emit(Op::OP_BITCAST, type, value);

    // The value is boxed when the bits above it are all ones, else it is the
    // canonical NaN
    const SemanticType valueType = SemanticType::integer(bits);
    SemanticOperand upper = emit(Op::OP_SHIFT_RIGHT_LOGICAL, registerType,
                                 value, getImmediate(bits, registerType));
    SemanticOperand isBoxed = m_block.newTemporary(SemanticType::integer(1));
    SemanticInstruction compare(Op::OP_COMPARE, registerType, isBoxed, upper,
                    getImmediate((int64)lowMask(64 - bits), registerType));
    compare.setCondition(Op::COND_EQUAL);
    m_block.append(compare);
    SemanticOperand low = emit(Op::OP_TRUNCATE, valueType, value);
    SemanticOperand unboxed = emit(Op::OP_SELECT, valueType, isBoxed, low,
                        getImmediate((int64)getCanonicalNan(type), valueType));
    return emit(Op::OP_BITCAST, type, unboxed);
}

void RiscvLiftContext::writeFloat(uint index, const SemanticOperand& value)
{
    const RiscvDecodedOperand& operand = getOperand(index);
    CHECK(operand.m_kind == RiscvDecodedOperand::KIND_REGISTER);
    CHECK(isFloatRegister(operand.m_register));
    const uint bits = value.getType().getTotalBits();
    CHECK((bits == 16) || (bits == 32) || (bits == 64));
    const SemanticType registerType = SemanticType::integer(64);
    SemanticOperand pattern = value;
    if (value.getType().getKind() != SemanticType::KIND_INTEGER)
        pattern = emit(Op::OP_BITCAST, SemanticType::integer(bits), value);
    if (bits < 64)
    {
        // The upper bits are set to ones
        SemanticOperand wide = emit(Op::OP_ZERO_EXTEND, registerType, pattern);
        pattern = emit(Op::OP_OR, registerType, wide,
                       getImmediate((int64)(~lowMask(bits)), registerType));
    }
    move(getRegister(operand.m_register), pattern);
}

SemanticOperand RiscvLiftContext::getAddress(uint index)
{
    CHECK(index < RiscvOpcode::MAX_OPERANDS);
    if (m_isAddressComputed[index])
        return m_addresses[index];

    const RiscvDecodedOperand& operand = getOperand(index);
    CHECK(operand.m_kind == RiscvDecodedOperand::KIND_MEMORY);
    const SemanticType type = getNativeType();

    SemanticOperand address;
    if (operand.m_base == RISCV_REG_X0)
    {
        address = getImmediate(operand.m_hasOffset ? operand.m_offset : 0, type);
    } else
    {
        address = getRegister(operand.m_base);
        if (operand.m_hasOffset && (operand.m_offset != 0))
            address = emit(Op::OP_ADD, type, address,
                           getImmediate(operand.m_offset, type));
        else
            // The instruction can change the base (ld a0, 0(a0)) before it
            // accesses all the memory: keep the value
            address = emit(Op::OP_MOVE, type, address);
    }
    m_addresses[index] = address;
    m_isAddressComputed[index] = true;
    return address;
}

/////////////////////////////////////////////////////////////////////////////
// RiscvSemanticLifter
/////////////////////////////////////////////////////////////////////////////

RiscvSemanticLifter::RiscvSemanticLifter(OpcodeSubsystems::DisassemblerType type,
                                         const ArchitectureDescriptionPtr& architecture) :
    m_type(type),
    m_architecture(architecture)
{
    CHECK((type == OpcodeSubsystems::DISASSEMBLER_RISCV_32) ||
          (type == OpcodeSubsystems::DISASSEMBLER_RISCV_64));
    if (m_architecture.getPointer() == NULL)
        m_architecture = ArchitectureDescriptionPtr(new RiscvArchitecture(
                (type == OpcodeSubsystems::DISASSEMBLER_RISCV_64) ?
                    RiscvInstructionSet::RISCV_RV64 :
                    RiscvInstructionSet::RISCV_RV32));
}

ArchitectureDescriptionPtr RiscvSemanticLifter::getArchitecture() const
{
    return m_architecture;
}

const RiscvOpcode* RiscvSemanticLifter::resolve(const RiscvOpcode& instruction,
                                                RiscvOpcodePtr& storage)
{
    const RiscvOpcode* current = &instruction;
    // A compressed instruction is lifted by its expansion
    if (current->isCompressed())
    {
        if (current->getExpansion().getPointer() == NULL)
            return NULL;
        storage = current->getExpansion();
        current = storage.getPointer();
    }
    // An alias by the general encoding: the decoder keeps the same bits
    // decoded without the aliases
    if (current->getNoAliasForm().getPointer() != NULL)
    {
        storage = current->getNoAliasForm();
        current = storage.getPointer();
    }
    return current;
}

const riscvdis::RiscvSemanticEntry* RiscvSemanticLifter::findEntry(
                                            const RiscvOpcode& instruction)
{
    RiscvOpcodePtr storage;
    const RiscvOpcode* resolved = resolve(instruction, storage);
    if (resolved == NULL)
        return NULL;

    const RiscvEncodingEntry* encoding = resolved->getEntry();
    const char* name = encoding->m_mnemonic;
    const uint operandsCount = resolved->getOperandsCount();

    // The area of the encoding entry decides the semantic table
    struct AreaTable {
        const RiscvEncodingEntry* m_encoding;
        const RiscvSemanticEntry* m_semantic;
    };
    const AreaTable areas[] = {
        { gRiscvBaseEncodingTable, gRiscvSemanticTable },
        { gRiscvVectorEncodingTable, gRiscvVectorSemanticTable },
        { gRiscvPrivilegedEncodingTable, gRiscvPrivilegedSemanticTable }
    };
    const RiscvSemanticEntry* table = NULL;
    for (uint i = 0; i < sizeof(areas) / sizeof(areas[0]); i++)
    {
        const RiscvEncodingEntry* end = areas[i].m_encoding;
        while (end->m_mnemonic != NULL)
            end++;
        if ((encoding >= areas[i].m_encoding) && (encoding < end))
        {
            table = areas[i].m_semantic;
            break;
        }
    }
    if (table == NULL)
        return NULL;

    for (const RiscvSemanticEntry* entry = table;
         entry->m_template.m_name != NULL; entry++)
    {
        if ((entry->m_operandsCount != RiscvSemanticEntry::ANY_OPERANDS_COUNT) &&
            (entry->m_operandsCount != operandsCount))
            continue;
        if (strcmp(entry->m_template.m_name, name) == 0)
            return entry;
    }
    return NULL;
}

OpcodeClassification RiscvSemanticLifter::classify(const OpcodePtr& instruction) const
{
    if (instruction->getType() != m_type)
        return OpcodeClassification();
    const RiscvOpcode& riscv = *(const RiscvOpcode*)instruction.getPointer();
    const riscvdis::RiscvSemanticEntry* entry = findEntry(riscv);
    if (entry == NULL)
        return OpcodeClassification();

    RiscvOpcodePtr storage;
    const RiscvOpcode* resolved = resolve(riscv, storage);
    const SemanticTemplate& semanticTemplate = entry->m_template;
    const uint count = resolved->getOperandsCount();
    uint32 properties = semanticTemplate.m_properties;
    uint32 units = semanticTemplate.m_units;

    // The memory the operands access
    bool hasMemoryOperand = false;
    for (uint i = 0; i < count; i++)
        if (resolved->getOperand(i).m_kind == RiscvDecodedOperand::KIND_MEMORY)
            hasMemoryOperand = true;
    const bool isImplicitMemory =
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
    if (semanticTemplate.m_flagsRead != 0)
        properties |= OpcodeClassification::PROPERTY_READS_FLAGS;
    if (semanticTemplate.m_flagsWritten != 0)
        properties |= OpcodeClassification::PROPERTY_WRITES_FLAGS;

    OpcodeClassification result(
                (OpcodeClassification::Category)semanticTemplate.m_category,
                units, properties);
    result.setFlags(semanticTemplate.m_flagsRead, semanticTemplate.m_flagsWritten,
                    semanticTemplate.m_flagsUndefined);

    // The size of the data: the entry gives the vector and the floating point
    // element, otherwise the first register operand
    if (entry->m_dataKind != SemanticType::KIND_VOID)
    {
        uint lanes = entry->m_dataLanes;
        if (lanes == 0)
        {
            // From vtype, at runtime: the registers' lanes at the widest
            lanes = m_architecture->getMaximumVectorBits() / entry->m_dataBits;
            if (lanes == 0)
                lanes = 1;
        }
        result.setDataType(SemanticType(
                    (SemanticType::Kind)entry->m_dataKind, entry->m_dataBits,
                    lanes, (SemanticType::FloatFormat)entry->m_dataFormat));
    } else if ((count > 0) &&
               (resolved->getOperand(0).m_kind ==
                RiscvDecodedOperand::KIND_REGISTER))
    {
        result.setDataType(SemanticType::integer(
                    m_architecture->getRegister(
                                resolved->getOperand(0).m_register).m_bits));
    } else
    {
        result.setDataType(SemanticType::integer(
                    m_architecture->getNativeIntegerBits()));
    }
    return result;
}

bool RiscvSemanticLifter::lift(const OpcodePtr& instruction,
                               SemanticBlock& block) const
{
    CHECK(block.getArchitecture() == m_type);

    ProcessorAddress address(gNullPointerProcessorAddress);
    instruction->getOpcodeAddress(address);
    block.beginGuestInstruction(address, instruction->getOpcodeSize());

    // An invalid instruction has no semantics
    const riscvdis::RiscvSemanticEntry* entry = NULL;
    const RiscvOpcode* riscv = NULL;
    if (instruction->getType() == m_type)
    {
        riscv = (const RiscvOpcode*)instruction.getPointer();
        entry = findEntry(*riscv);
    }
    if (entry == NULL)
    {
        block.append(SemanticInstruction(Op::OP_UNDEFINED, SemanticType(),
                                         SemanticOperand()));
        return false;
    }

    RiscvLiftContext context(*riscv, block, m_architecture);
    if (entry->m_lift != NULL)
        entry->m_lift(context);
    else
        instantiate(entry->m_template, context);
    return true;
}

namespace {

// The most temporaries and labels a template uses
enum { MAX_TEMPLATE_VALUES = 8 };

/*
 * The state of the instantiation of one template: its temporaries and labels
 * (created when they're first used).
 */
class TemplateInstance {
public:
    explicit TemplateInstance(RiscvLiftContext& context) :
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
        {
            const RiscvDecodedOperand& operand = m_context.getOperand(type.m_bits);
            if (operand.m_kind == RiscvDecodedOperand::KIND_REGISTER)
                return SemanticType::integer(
                        m_context.getArchitecture()->getRegister(
                                            operand.m_register).m_bits);
            return m_context.getNativeType();
        }
        case SemanticTemplateType::SOURCE_DOUBLE_OPERAND:
            return SemanticType::integer(m_context.getXlen() * 2);
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
                RiscvDecodedOperand::KIND_MEMORY)
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
                        (uint64)reference.m_value), m_context.getNativeType());
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

        switch (operation)
        {
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

        // The result of a compare is a boolean
        const bool isBoolean = operation == SemanticOperation::OP_COMPARE;
        SemanticOperand result = m_context.newTemporary(
                        isBoolean ? SemanticType::integer(1) : type);
        SemanticInstruction instruction(operation, type, result,
                                        sources[0], sources[1], sources[2]);
        instruction.setCondition((SemanticOperation::Condition)step.m_condition);
        m_context.emit(instruction);
        writeReference(step.m_destination, result);
    }

    RiscvLiftContext& m_context;
    SemanticOperand m_temporaries[MAX_TEMPLATE_VALUES];
    SemanticOperand m_labels[MAX_TEMPLATE_VALUES];
    SemanticOperand m_loaded[RiscvOpcode::MAX_OPERANDS];
};

} // namespace

void RiscvSemanticLifter::instantiate(const SemanticTemplate& semanticTemplate,
                                      RiscvLiftContext& context) const
{
    TemplateInstance instance(context);
    instance.run(semanticTemplate);
}
