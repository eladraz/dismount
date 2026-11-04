#ifndef __TBA_DISMOUNT_PROC_ARM_ARMNOTATION_H
#define __TBA_DISMOUNT_PROC_ARM_ARMNOTATION_H

/*
 * ArmNotation.h
 *
 * Format a decoded ARM instruction in the LLVM syntax (the unified syntax of
 * AArch32, the A64 syntax), the text which objdump / llvm-objdump print and
 * the integrated assembler of clang reads.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "dismount/OpcodeFormatter.h"
#include "dismount/OpcodeDataFormatter.h"
#include "dismount/proc/arm/ArmOpcode.h"

/*
 * The text of an ARM instruction: the full mnemonic (ArmOpcode::
 * getMnemonic()), a space, and the operands separated by ", ".
 *
 *   - Registers by the LLVM names, vectors with their arrangement
 *     ("v0.4s", "z1.d", "p0/m", "za0h.s[w12, 0]"), lists in braces ("{ v0.4s,
 *     v1.4s }" as LLVM prints A64 lists, "{r0, r1, lr}" for AArch32).
 *   - Immediates as LLVM prints them: "#16", "#-4", "#0x10" for the hex
 *     ones (ArmDecodedOperand::m_isHex), "#1.50000000" for the A64 floating
 *     point, "#1.500000e+00" for AArch32, the shifts ", lsl #12".
 *   - Memory as "[x0, #8]", "[x0, #8]!", "[x0], #8", "[x0, x1, lsl #3]",
 *     "[r0, -r1]", "[r0:128]".
 *   - A branch target as the absolute address in hex without '#' ("0x1000",
 *     what objdump prints after the instruction's address is applied); the
 *     tests disassemble at the address of the objdump listing.
 *
 * The exact rules are the ones of LLVM, checked by tests/arm/run_compat.py.
 */
class ArmNotation : public OpcodeFormatter {
public:
    /*
     * Constructor
     *
     * instruction   - The instruction, an ArmOpcode (the formatter keeps a
     *                 reference of it, as the other formatters do)
     * dataFormatter - Used for the separator and for endInstruction() only:
     *                 the numbers are formatted in the LLVM syntax
     */
    ArmNotation(const OpcodePtr& instruction, OpcodeDataFormatter& dataFormatter);

    // See OpcodeFormatter
    virtual cString string(OpcodeFormatStruct* formatStruct = NULL) const;
    virtual uint parseOperandAddress(ProcessorAddress& address);

    /*
     * Format one operand
     *
     * type    - The instruction set (the register names)
     * operand - The operand
     */
    static cString formatOperand(ArmInstructionSet::DisassemblerTypes type,
                                 const ArmDecodedOperand& operand);

private:
    // The instruction, and its ArmOpcode view
    OpcodePtr m_data;
    const ArmOpcode* m_opcode;
    // The GUI formatter
    OpcodeDataFormatter& m_dataFormatter;
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARMNOTATION_H
