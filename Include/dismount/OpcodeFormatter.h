#ifndef __TBA_DISMOUNT_OPCODEFORMATTER_H
#define __TBA_DISMOUNT_OPCODEFORMATTER_H

/*
 * OpcodeFormatter.h
 *
 * This class is responsible for formatting a decoded instruction into a series
 * meaningful human readable strings. In cooperation with the OpcodeDataFormatter
 * class (which responsible the GUI behaviour) the result is a single string
 * which can be viewed to the user.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/string.h"

/*
 * Implemented by the different platform.
 *
 * IMPLEMENTATION NOTE: Don't forget to implement "InvalidOpcodeByte".
 */
class OpcodeFormatter {
public:
    /*
     * Virtual destructor. You can inherit from me.
     */
    virtual ~OpcodeFormatter();

    /*
     * This struct define the position of the different part of the instruction.
     *
     * Each instruction may have the following pattern:
     *    <prefix><instruction><operands>
     *
     * Using the struct values to retrieve the different parts of the opcode
     *   Position 0 to 'm_opcodeNameStart' is used for the prefix
     *   Position m_opcodeNameStart to 'm_opcodeOperandsStart' is used for the
     *            instruction name
     *   Position m_opcodeOperandsStart until the end is used for the operands
     */
    struct OpcodeFormatStruct {
        // The starting offset of the opcode name
        uint m_opcodeNameStart;
        // The starting offset of the opcode operands
        uint m_opcodeOperandsStart;

        /*
         * Take instruction string and return only the prefix and the
         * instruction name.
         *
         * opcode - The string returned by the 'string()' API call
         */
        cString getInstructionAndPrefix(const cString& opcode) const;

        /*
         * Take instruction string and return only the operands part
         *
         * opcode - The string returned by the 'string()' API call
         */
        cString getOperands(const cString& opcode) const;
    };

    /*
     * Return a string which describes the opcode
     */
    virtual cString string(OpcodeFormatStruct* formatStruct = NULL) const = 0;

    /*
     * Parses the address from an opcode operand (used for
     * jumping to that address)
     *
     * If the operand is a valid address, fills the
     * address object with the parsed address.
     * If the operand is not an address (a register, for
     * instance) - fills it with address 0
     *
     * address - The address to fill
     *
     * Returns the operand address type for a valid address, or a
     * "no type" for operands that are not addresses
     */
    virtual uint parseOperandAddress(ProcessorAddress& address) = 0;
};
// Reference countable object
typedef cSmartPtr<OpcodeFormatter> OpcodeFormatterPtr;

#endif // __TBA_DISMOUNT_OPCODEFORMATTER_H
