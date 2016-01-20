#include "dismount/dismount.h"
/*
 * DefaultOpcodeDataFormatter.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/char.h"
#include "xStl/data/string.h"
#include "xStl/data/datastream.h"
#include "dismount/DefaultOpcodeDataFormatter.h"

const character DefaultOpcodeDataFormatter::m_hexadecimalSuffix =
    XSTL_CHAR('h');

DefaultOpcodeDataFormatter::DefaultOpcodeDataFormatter(uint opcodeNameAlignment) :
    m_opcodeNameAlignment(opcodeNameAlignment)
{
}

void DefaultOpcodeDataFormatter::newInstruction(bool,
                                                const ProcessorAddress&)
{
}

cString DefaultOpcodeDataFormatter::endInstruction(const cString& instruction)
{
    return instruction;
}

cString DefaultOpcodeDataFormatter::translateUint8(uint8 data)
{
    return HEXBYTE(data) + m_hexadecimalSuffix;
}

cString DefaultOpcodeDataFormatter::translateUint16(uint16 data)
{
    return HEXWORD(data) + m_hexadecimalSuffix;
}

cString DefaultOpcodeDataFormatter::translateUint32(uint32 data)
{
    return HEXDWORD(data) + m_hexadecimalSuffix;
}

cString DefaultOpcodeDataFormatter::translateUint64(uint64 data)
{
    return HEXQWORD(data) + m_hexadecimalSuffix;
}

cString DefaultOpcodeDataFormatter::translateRelativeDisplacement(int64 displacement)
{
    if (displacement < 0)
    {
        return cString(" - ") + HEXNUMBER((uint)(-displacement))  + m_hexadecimalSuffix;
    } else
    {
        return cString(" + ") + HEXNUMBER((uint)displacement) + m_hexadecimalSuffix;
    }
}

cString DefaultOpcodeDataFormatter::translateAbsoluteAddress(
    const ProcessorAddress& absoulte)
{
    switch (absoulte.getAddressType())
    {
    case ProcessorAddress::PROCESSOR_16:
        return HEXWORD((uint16)absoulte.getAddress()) + m_hexadecimalSuffix;
    case ProcessorAddress::PROCESSOR_20:
        return HEXWORD((uint16)(absoulte.getAddress() >> 16)) +
               ":" +
               HEXWORD((uint16)(absoulte.getAddress() & 0xFFFF));
    case ProcessorAddress::PROCESSOR_32:
        return HEXDWORD((uint32)absoulte.getAddress()) + m_hexadecimalSuffix;
    case ProcessorAddress::PROCESSOR_64:
        return HEXQWORD(absoulte.getAddress()) + m_hexadecimalSuffix;
    default:
        CHECK_FAIL();
    }
}

cString DefaultOpcodeDataFormatter::translateRelativeAddress(
    ProcessorAddress::intAddress relative,
    bool isIpValid,
    const ProcessorAddress& ip)
{
    // Get the absolute-address
    if (isIpValid)
        return translateAbsoluteAddress(ip + relative);

    // Show the relative address
    cString ret;

    if (relative >= 0)
        return cString("$+") + HEXNUMBER((uint32)relative);
    else
        return cString("$-") + HEXNUMBER((uint32)(-relative));
}

cString DefaultOpcodeDataFormatter::getOpcodesSeparator(const cString&)
{
    return ", ";
}

cString DefaultOpcodeDataFormatter::reparseOpcode(const cString& opcodeName)
{
    cString ret(opcodeName);
    if (ret.length() <= m_opcodeNameAlignment)
    {
        ret+= cString::dup(" ", m_opcodeNameAlignment - ret.length());
    } else
    {
        // ret = ret.left(m_opcodeNameAlignment);
        ret+= ' ';
    }
    return ret;
}



cString DefaultOpcodeDataFormatter::reparseFirstOperand(const cString& opcodeName)
{
    return opcodeName;
}
cString DefaultOpcodeDataFormatter::reparseSecondOperand(const cString& opcodeName)
{
    return opcodeName;
}
cString DefaultOpcodeDataFormatter::reparseThirdOperand(const cString& opcodeName)
{
    return opcodeName;
}
