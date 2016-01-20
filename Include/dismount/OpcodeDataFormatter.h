#ifndef __TBA_DISMOUNT_OPCODEDATAFORMATTER_H
#define __TBA_DISMOUNT_OPCODEDATAFORMATTER_H

/*
 * OpcodeDataFormatter.h
 *
 * The opcode-data-formatter is used in order to match the opcode into the
 * GUI format.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "dismount/ProcessorAddress.h"

/*
 * Formats the opcode to match the GUI requirements.
 * For examples:
 * - The opcode "B8 11 22 33 44" is "mov eax," and the number might be formatted
 *   in any forms: "44332211h" or "0x44332211" binary or even be matched with a
 *   symbol: "gFastQueryMutex"
 * - The opcode "74 0B" is "jz" and can be formatted as "jz $+0B". If the current
 *   address is 103, then the opcode means "jz 110"
 *
 * NOTE: This class is not thread-safe, and might be called from a different
 *       number of threads. The usages of multi-threads depends how the
 *       programmer uses the "StreamDisassmbler". See StreamDisassmblerFactory
 *       documentation header for more information.
 */
class OpcodeDataFormatter {
public:
    /*
     * Virtual dtor, you can inherit from me
     */
    virtual ~OpcodeDataFormatter() {};

    // Disassembler callbacks

    /*
     * Called upon new view of an instruction. Used to clear any previous
     * allocated information.
     *
     * isIpValid - Set to true if 'ip' is the valid instruction pointer
     *             offset, otherwise the 'ip' value is invalid.
     * ip        - The address of the instruction.
     */
    virtual void newInstruction(bool isIpValid,
                                const ProcessorAddress& ip) = 0;
    /*
     * Called in order to finalize the instruction format.
     *
     * instruction - The string which build by the viewer.
     */
    virtual cString endInstruction(const cString& instruction) = 0;

    // Instruction reference access formatter.

    /*
     * Used to convert an absolute number into a string. For example, this
     * function will be called for translating "mov  al, 13h", in this case
     * the function will get 0x13 in data argument and returns 13h.
     *
     * data - The number to be translated.
     *
     * This function
     * NOTE: The implementation can output the number as 8 bit number in decimal
     *       mode, hex-mode or binary mode.
     */
    virtual cString translateUint8(uint8 data) = 0;
    virtual cString translateUint16(uint16 data) = 0;
    virtual cString translateUint32(uint32 data) = 0;
    virtual cString translateUint64(uint64 data) = 0;

    /*
     * Translate a displacement into a string. The displacement might often be
     * used to pointer into stack variables...
     */
    virtual cString translateRelativeDisplacement(int64 displacement) = 0;

    /*
     * Translates absolute address into a string.
     *
     * For example, use this function to attach a import table function thier
     * real names.
     */
    virtual cString translateAbsoluteAddress(const ProcessorAddress& absoulte) = 0;

    /*
     * Translates relative address into a string.
     *
     * relative  - The relative address
     * isIpValid - Set to true if 'ip' is the valid instruction pointer
     *             offset, otherwise the 'ip' value is invalid.
     * ip        - The address of the instruction.
     *
     * THREAD_SAFE NOTE:
     *     Both 'isIpValid' and 'ip' arguments are duplicated and also supplied
     *     at the constructor. The reason for this duplication is to avoid
     *     storing information in the global class space since it might be used
     *     in number of different threads.
     *
     * IMPLEMENTATION NOTE:
     *       It's recommended that the returned value will be sent to
     *       'translateAbsolduteAddress' for second symbol mathing.
     */
    virtual cString translateRelativeAddress(
        ProcessorAddress::intAddress relative,
        bool isIpValid,
        const ProcessorAddress& ip) = 0;


    // Instruction design

    /*
     * Returns the default opcode separator. Usually returns the string: ", ".
     *
     * instructionSoFar - The string which was build until now.
     */
    virtual cString getOpcodesSeparator(const cString& instructionSoFar) = 0;

    /*
     * For example, most often the implementation of this function will be used
     * to align the opcode instruction to match the colum size.
     */
    virtual cString reparseOpcode(const cString& opcodeName) = 0;

    /*
     * NOTE: This function are called after all 'translate *' API.
     */
    virtual cString reparseFirstOperand(const cString& opcodeName) = 0;
    virtual cString reparseSecondOperand(const cString& opcodeName) = 0;
    virtual cString reparseThirdOperand(const cString& opcodeName) = 0;
};

#endif // __TBA_DISMOUNT_OPCODEDATAFORMATTER_H
