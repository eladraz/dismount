#include "dismount/dismount.h"
/*
 * OpcodeFormatter.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "dismount/OpcodeFormatter.h"

OpcodeFormatter::~OpcodeFormatter()
{
}

cString OpcodeFormatter::OpcodeFormatStruct::getInstructionAndPrefix(
                                            const cString& opcode) const
{
    return opcode.left(m_opcodeOperandsStart).trim();
}

cString OpcodeFormatter::OpcodeFormatStruct::getOperands(
                                            const cString& opcode) const
{
    return opcode.right(opcode.length() - m_opcodeOperandsStart);
}
