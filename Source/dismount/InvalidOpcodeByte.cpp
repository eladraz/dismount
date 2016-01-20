#include "dismount/dismount.h"
/*
 * InvalidOpcodeByte.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/Opcode.h"
#include "dismount/InvalidOpcodeByte.h"

InvalidOpcodeByte::InvalidOpcodeByte(uint8 invalidOpcode,
                                     bool shouldUseAddress,
                                     const ProcessorAddress& opcodeAddress) :
    m_invalidOpcode(invalidOpcode),
    m_shouldUseAddress(shouldUseAddress),
    m_opcodeAddress(opcodeAddress)
{
}

OpcodeSubsystems::DisassemblerType InvalidOpcodeByte::getType() const
{
    return OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE;
}

cBuffer InvalidOpcodeByte::getOpcode() const
{
    return cBuffer(&m_invalidOpcode, 1);
}

uint InvalidOpcodeByte::getOpcodeSize() const
{
    return 1;
}

bool InvalidOpcodeByte::getOpcodeAddress(ProcessorAddress& address) const
{
    if (!m_shouldUseAddress)
        return false;

    address = m_opcodeAddress;
    return true;
}

bool InvalidOpcodeByte::isBranch() const
{
    return false;
}

bool InvalidOpcodeByte::isSwitch() const
{
    return false;
}

uint32 InvalidOpcodeByte::getSwitchTableOffset() const
{
    return 0;
}

int InvalidOpcodeByte::getAlterProperty() const
{
    return Opcode::FLOW_NO_ALTER;
}
