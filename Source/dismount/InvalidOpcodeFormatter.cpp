#include "dismount/dismount.h"
/*
 * InvalidOpcodeFormatter.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/OpcodeDataFormatter.h"
#include "dismount/InvalidOpcodeFormatter.h"

const character* InvalidOpcodeFormatter::gInvalidOpcodeStringName =
    XSTL_STRING("DB");

InvalidOpcodeFormatter::InvalidOpcodeFormatter(
        const OpcodePtr& instruction,
        OpcodeDataFormatter& dataFormatter) :
    m_dataFormatter(dataFormatter),
    m_data(instruction)
{
    CHECK(!instruction.isEmpty());
    CHECK(instruction->getType() ==
          OpcodeSubsystems::DISASSEMBLER_INVALID_OPCODE);
    ASSERT(instruction->getOpcodeSize() == 1);
}

cString InvalidOpcodeFormatter::string(OpcodeFormatStruct* formatStruct) const
{
    // Get the instruction address
    ProcessorAddress ipAddress(gNullPointerProcessorAddress);
    bool shouldUseIp = m_data->getOpcodeAddress(ipAddress);

    // Mark new instruction
    m_dataFormatter.newInstruction(shouldUseIp, ipAddress);

    // Format instruction
    if (formatStruct != NULL)
        formatStruct->m_opcodeNameStart = 0;

    cString ret = m_dataFormatter.reparseOpcode(gInvalidOpcodeStringName);

    if (formatStruct != NULL)
        formatStruct->m_opcodeOperandsStart = ret.length();

    ret+= m_dataFormatter.reparseFirstOperand(
        m_dataFormatter.translateUint8(m_data->getOpcode()[0]));

    // Build the instruction
    return m_dataFormatter.endInstruction(ret);
}

uint InvalidOpcodeFormatter::parseOperandAddress(ProcessorAddress& address)
{
    address = ProcessorAddress(gNullPointerProcessorAddress);
    return 0;
}
