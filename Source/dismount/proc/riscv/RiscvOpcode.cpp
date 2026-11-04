#include "dismount/dismount.h"
/*
 * RiscvOpcode.cpp
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
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"

RiscvDecodedOperand::RiscvDecodedOperand() :
    m_kind(KIND_NONE),
    m_register(0),
    m_immediate(0),
    m_fliIndex(0),
    m_base(0),
    m_offset(0),
    m_hasOffset(false),
    m_csr(0),
    m_roundingMode(0),
    m_fenceSet(0),
    m_vtype(0),
    m_isHex(false),
    m_isUnsigned(false)
{
}

RiscvOpcode::RiscvOpcode(RiscvInstructionSet::DisassemblerTypes type,
                         bool shouldUseAddress,
                         const ProcessorAddress& opcodeAddress,
                         uint32 encoding,
                         uint size,
                         const riscvdis::RiscvEncodingEntry* entry) :
    m_type(type),
    m_shouldUseAddress(shouldUseAddress),
    m_opcodeAddress(opcodeAddress),
    m_encoding(encoding),
    m_size(size),
    m_entry(entry),
    m_operandsCount(0)
{
    CHECK(entry != NULL);
    CHECK((size == 2) || (size == 4));
}

OpcodeSubsystems::DisassemblerType RiscvOpcode::getType() const
{
    return RiscvInstructionSet::toSubsystem(m_type);
}

cBuffer RiscvOpcode::getOpcode() const
{
    // Little endian
    uint8 bytes[4];
    for (uint i = 0; i < m_size; i++)
        bytes[i] = (uint8)(m_encoding >> (8 * i));
    return cBuffer(bytes, m_size);
}

uint RiscvOpcode::getOpcodeSize() const
{
    return m_size;
}

bool RiscvOpcode::getOpcodeAddress(ProcessorAddress& address) const
{
    if (!m_shouldUseAddress)
        return false;

    address = m_opcodeAddress;
    return true;
}

bool RiscvOpcode::isBranch() const
{
    int flow = getAlterProperty();
    return (flow != FLOW_NO_ALTER) &&
           ((flow & (FLOW_INVALID | FLOW_DEBUG)) == 0);
}

bool RiscvOpcode::isSwitch() const
{
    return false;
}

uint32 RiscvOpcode::getSwitchTableOffset() const
{
    return 0;
}

int RiscvOpcode::getAlterProperty() const
{
    return m_entry->m_flow;
}

RiscvInstructionSet::DisassemblerTypes RiscvOpcode::getInstructionSet() const
{
    return m_type;
}

uint32 RiscvOpcode::getEncoding() const
{
    return m_encoding;
}

const riscvdis::RiscvEncodingEntry* RiscvOpcode::getEntry() const
{
    return m_entry;
}

uint64 RiscvOpcode::getAddress() const
{
    if (!m_shouldUseAddress)
        return 0;
    return m_opcodeAddress.getAddress();
}

bool RiscvOpcode::isCompressed() const
{
    return m_size == 2;
}

const RiscvOpcodePtr& RiscvOpcode::getExpansion() const
{
    return m_expansion;
}

void RiscvOpcode::setExpansion(const RiscvOpcodePtr& expansion)
{
    m_expansion = expansion;
}

const RiscvOpcodePtr& RiscvOpcode::getNoAliasForm() const
{
    return m_noAliasForm;
}

void RiscvOpcode::setNoAliasForm(const RiscvOpcodePtr& form)
{
    m_noAliasForm = form;
}

uint RiscvOpcode::getOperandsCount() const
{
    return m_operandsCount;
}

const RiscvDecodedOperand& RiscvOpcode::getOperand(uint index) const
{
    if (index >= m_operandsCount)
        XSTL_THROW(cException, XSTL_STRING("no such operand"), EXCEPTION_FAILED);
    return m_operands[index];
}

void RiscvOpcode::addOperand(const RiscvDecodedOperand& operand)
{
    CHECK(m_operandsCount < MAX_OPERANDS);
    m_operands[m_operandsCount] = operand;
    m_operandsCount++;
}

const cString& RiscvOpcode::getMnemonic() const
{
    return m_mnemonic;
}

void RiscvOpcode::setMnemonic(const cString& mnemonic)
{
    m_mnemonic = mnemonic;
}
