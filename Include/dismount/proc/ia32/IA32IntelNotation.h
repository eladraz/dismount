#ifndef __TBA_DISMOUNTI_PROC_IA32_IA32INTELNOTATION_H
#define __TBA_DISMOUNTI_PROC_IA32_IA32INTELNOTATION_H

/*
 * IA32IntelNotation.h
 *
 * Translate IA32Opcode into a human readable string.
 * The returned string is formated in a intel-assembly language notation, as
 * opposite to gnu-notation.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/smartptr.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeFormatter.h"
#include "dismount/OpcodeDataFormatter.h"
#include "dismount/proc/ia32/IA32Opcode.h"

/*
 * Translate IA32Opcode into a human readable string.
 */
class IA32IntelNotation : public OpcodeFormatter {
public:
    /*
     * Constructor.
     *
     * instruction - The IA32Opcode instruction.
     * dataFormatter - The
     *
     * Throw exception if instruction is not in the IA32Opcode.
     * NOTE: If the instruction is InvalidOpcodeByte then the
     *       InvalidOpcodeFormatter should be instance
     */
    IA32IntelNotation(const OpcodePtr& instruction,
                      OpcodeDataFormatter& dataFormatter);

    /*
     * See OpcodeFormatter::string()
     * Return intel-notation assembly language:
     *    <opcode> <first-operand><second-operand>
     *
     * [Without all the "opcode.l @blabla" crap]
     */
    virtual cString string(OpcodeFormatStruct* formatStruct = NULL) const;

    /*
     * See OpcodeFormatter::parseOperandAddress()
     */
    uint parseOperandAddress(ProcessorAddress& address);

private:
    /*
     * Return true if the disassembler is 32bit instruction set
     */
    bool is32bit() const;

    /*
     * Translate the operand into a string
     */
    cString stringOperand(ia32dis::OperandType type) const;

    /*
     * According to the mod/rm, processor mode, builds a string
     * describing the register which is accessed.
     *
     * type - Used to determine what is the bus usage in the command
     */
    cString getModrmString(ia32dis::OperandType type) const;

    /*
     * Build up a string which indicate which segment should be used.
     * Return empty string if the default selector should be used
     */
    cString getSegmentSelector() const;

    // The data-formatter to be used.
    OpcodeDataFormatter& m_dataFormatter;
    // The byte to be shown.
    OpcodePtr m_data;
    // Cast pointer to the *m_data.
    IA32Opcode* m_opcode;
};

#endif // __TBA_DISMOUNTI_PROC_IA32_IA32INTELNOTATION_H
