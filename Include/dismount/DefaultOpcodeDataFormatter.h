#ifndef __TBA_DISMOUNT_DEFAULTOPCODEDATAFORMATTER_H
#define __TBA_DISMOUNT_DEFAULTOPCODEDATAFORMATTER_H

/*
 * DefaultOpcodeDataFormatter.h
 *
 * Default implementation to the OpcodeDataFormatter, the immediate are shown
 * in thier nature view
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "dismount/OpcodeDataFormatter.h"

/*
 * The opcode will be formated in the following:
 *     <opcode-name><spaces><operand>,<operand>...
 * The opcode-name filled has a length given in the constructor -
 * "opcodeNameAlignment".
 * All the number are displaied in hexadecimal form.
 * If IP-address is supplied then all relative address are translated to
 * absolute addresses, otherwise they are shown in the format $+-ADDR.
 */
class DefaultOpcodeDataFormatter : public OpcodeDataFormatter {
public:
    /*
     * Constructor.
     *
     * opcodeNameAlignment - The number of character to be dedicating to the
     *                       opcode name colum. If the opcode name has more
     *                       characters then the 'opcodeNameAlignment' then the
     *                       opcode name will be cut, otherwise space character
     *                       will be appended to the opcode (Left alignment).
     */
    DefaultOpcodeDataFormatter(uint opcodeNameAlignment);

    /*
     * See OpcodeDataFormatter::newInstruction
     */
    virtual void newInstruction(bool isIpValid,
                                const ProcessorAddress& ip);

    /*
     * See OpcodeDataFormatter::endInstruction
     */
    virtual cString endInstruction(const cString& instruction);

    /*
     * See OpcodeDataFormatter::translateUint*
     */
    virtual cString translateUint8(uint8 data);
    virtual cString translateUint16(uint16 data);
    virtual cString translateUint32(uint32 data);
    virtual cString translateUint64(uint64 data);

    /*
     * See OpcodeDataFormatter::translateRelativeDisplacement
     */
    virtual cString translateRelativeDisplacement(int64 displacement);

    /*
     * See OpcodeDataFormatter::translateAbsoluteAddress
     */
    virtual cString translateAbsoluteAddress(const ProcessorAddress& absoulte);

    /*
     * See OpcodeDataFormatter::getOpcodesSeparator
     */
    virtual cString getOpcodesSeparator(const cString& instructionSoFar);

    /*
     * See OpcodeDataFormatter::translateRelativeAddress
     */
    virtual cString translateRelativeAddress(
        ProcessorAddress::intAddress relative,
        bool isIpValid,
        const ProcessorAddress& ip);

    /*
     * See OpcodeDataFormatter::reparse*
     */
    virtual cString reparseOpcode(const cString& opcodeName);
    virtual cString reparseFirstOperand(const cString& opcodeName);
    virtual cString reparseSecondOperand(const cString& opcodeName);
    virtual cString reparseThirdOperand(const cString& opcodeName);


private:
    // The number of character for the alignment
    uint m_opcodeNameAlignment;
    // The suffix for hexadecimal number
    static const character m_hexadecimalSuffix;
};

// The reference countable object
typedef cSmartPtr<DefaultOpcodeDataFormatter> DefaultOpcodeDataFormatterPtr;

#endif // __TBA_DISMOUNT_DEFAULTOPCODEDATAFORMATTER_H
