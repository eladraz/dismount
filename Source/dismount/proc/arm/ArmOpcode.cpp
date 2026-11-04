#include "dismount/dismount.h"
/*
 * ArmOpcode.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/string.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmOperandCodec.h"
#include "dismount/proc/arm/ArmOperandCodecA32.h"

ArmDecodedOperand::ArmDecodedOperand() :
    m_kind(KIND_NONE),
    m_register(armdis::ARM_NO_REGISTER),
    m_arrangement(ARRANGEMENT_NONE),
    m_elementIndex(-1),
    m_predicateQualifier(PREDICATE_NONE),
    m_isPredicateCounter(false),
    m_count(0),
    m_stride(0),
    m_registerMask(0),
    m_immediate(0),
    m_floatImmediate(0),
    m_shift(SHIFT_NONE),
    m_shiftAmount(0),
    m_shiftRegister(armdis::ARM_NO_REGISTER),
    m_base(armdis::ARM_NO_REGISTER),
    m_index(armdis::ARM_NO_REGISTER),
    m_indexArrangement(ARRANGEMENT_NONE),
    m_offset(0),
    m_addressing(ADDRESSING_OFFSET),
    m_isSubtract(false),
    m_postIndexRegister(armdis::ARM_NO_REGISTER),
    m_alignment(0),
    m_condition(0),
    m_systemRegister(0),
    m_option(0),
    m_optionScheme(0),
    m_tile(0),
    m_sliceDirection(SLICE_NONE),
    m_sliceRegister(armdis::ARM_NO_REGISTER),
    m_sliceOffset(0),
    m_sliceOffsetLast(0),
    m_vectorGroup(0),
    m_isHex(false),
    m_isExplicit(false),
    m_isUnsigned(false),
    m_hasHash(false)
{
}

ArmOpcode::ArmOpcode(ArmInstructionSet::DisassemblerTypes type,
                     bool shouldUseAddress,
                     const ProcessorAddress& opcodeAddress,
                     uint32 encoding,
                     uint size,
                     const armdis::ArmEncodingEntry* entry,
                     uint condition,
                     uint itState) :
    m_type(type),
    m_shouldUseAddress(shouldUseAddress),
    m_opcodeAddress(opcodeAddress),
    m_encoding(encoding),
    m_size(size),
    m_entry(entry),
    m_condition(condition),
    m_itState(itState),
    m_operandsCount(0)
{
    CHECK(entry != NULL);
    CHECK((size == 2) || (size == 4));
}

OpcodeSubsystems::DisassemblerType ArmOpcode::getType() const
{
    return ArmInstructionSet::toSubsystem(m_type);
}

cBuffer ArmOpcode::getOpcode() const
{
    // The instructions are little endian. The T32 halfwords are in order: hw1
    // (the high 16 bits of the encoding) first
    uint8 bytes[4];
    if (m_size == 2)
    {
        bytes[0] = (uint8)(m_encoding);
        bytes[1] = (uint8)(m_encoding >> 8);
    } else if (m_type == ArmInstructionSet::ARM_T32)
    {
        bytes[0] = (uint8)(m_encoding >> 16);
        bytes[1] = (uint8)(m_encoding >> 24);
        bytes[2] = (uint8)(m_encoding);
        bytes[3] = (uint8)(m_encoding >> 8);
    } else
    {
        bytes[0] = (uint8)(m_encoding);
        bytes[1] = (uint8)(m_encoding >> 8);
        bytes[2] = (uint8)(m_encoding >> 16);
        bytes[3] = (uint8)(m_encoding >> 24);
    }
    return cBuffer(bytes, m_size);
}

uint ArmOpcode::getOpcodeSize() const
{
    return m_size;
}

bool ArmOpcode::getOpcodeAddress(ProcessorAddress& address) const
{
    if (!m_shouldUseAddress)
        return false;

    address = m_opcodeAddress;
    return true;
}

bool ArmOpcode::isBranch() const
{
    int flow = getAlterProperty();
    return (flow != FLOW_NO_ALTER) &&
           ((flow & (FLOW_INVALID | FLOW_DEBUG)) == 0);
}

bool ArmOpcode::isSwitch() const
{
    return false;
}

uint32 ArmOpcode::getSwitchTableOffset() const
{
    return 0;
}

int ArmOpcode::getAlterProperty() const
{
    return m_entry->m_flow;
}

ArmInstructionSet::DisassemblerTypes ArmOpcode::getInstructionSet() const
{
    return m_type;
}

uint32 ArmOpcode::getEncoding() const
{
    return m_encoding;
}

const armdis::ArmEncodingEntry* ArmOpcode::getEntry() const
{
    return m_entry;
}

uint64 ArmOpcode::getAddress() const
{
    if (!m_shouldUseAddress)
        return 0;
    return m_opcodeAddress.getAddress();
}

uint ArmOpcode::getCondition() const
{
    return m_condition;
}

uint ArmOpcode::getItState() const
{
    return m_itState;
}

bool ArmOpcode::isInItBlock() const
{
    return m_itState != 0;
}

bool ArmOpcode::isLastInItBlock() const
{
    // ITSTATE<3:0> is 1000 in the last instruction of the block
    return (m_itState != 0) && ((m_itState & 0xF) == 0x8);
}

bool ArmOpcode::isSettingFlags() const
{
    if ((m_entry->m_flags & armdis::ARM_ENTRY_SETS_FLAGS) != 0)
        return true;
    // The T16 instructions which set the flags only outside of an it block
    if ((m_entry->m_flags & armdis::ARM_ENTRY_T16_FLAGS_OUTSIDE_IT) != 0)
        return m_itState == 0;
    // The S bit of A32 / T32
    for (uint i = 0; i < armdis::ARM_MAX_OPERAND_SPECS; i++)
    {
        const armdis::ArmOperandSpec& spec = m_entry->m_operands[i];
        if (spec.m_kind == armdis::ARM_KIND_NONE)
            break;
        if (spec.m_kind == armdis::ARM_KIND_SETS_FLAGS)
        {
            // The T16 "adds": outside of an it block
            if ((spec.m_flags & armdis::ARM_A32_SETS_FLAGS_OUTSIDE_IT) != 0)
                return m_itState == 0;
            return ((m_encoding >> spec.m_position) & 1) != 0;
        }
    }
    return false;
}

uint ArmOpcode::getOperandsCount() const
{
    return m_operandsCount;
}

const ArmDecodedOperand& ArmOpcode::getOperand(uint index) const
{
    if (index >= m_operandsCount)
        XSTL_THROW(cException, XSTL_STRING("no such operand"), EXCEPTION_FAILED);
    return m_operands[index];
}

void ArmOpcode::addOperand(const ArmDecodedOperand& operand)
{
    CHECK(m_operandsCount < MAX_OPERANDS);
    m_operands[m_operandsCount] = operand;
    m_operandsCount++;
}

const cString& ArmOpcode::getMnemonic() const
{
    return m_mnemonic;
}

void ArmOpcode::setMnemonic(const cString& mnemonic)
{
    m_mnemonic = mnemonic;
}
