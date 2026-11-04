#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVNOTATION_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVNOTATION_H

/*
 * RiscvNotation.h
 *
 * Format a decoded RISC-V instruction in the LLVM syntax, the text which
 * llvm-objdump prints and llvm-mc reads.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "dismount/OpcodeFormatter.h"
#include "dismount/OpcodeDataFormatter.h"
#include "dismount/proc/riscv/RiscvOpcode.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"

/*
 * The text of a RISC-V instruction: the full mnemonic (RiscvOpcode::
 * getMnemonic()), a space, and the operands separated by ", ".
 *
 *   - Registers by the ABI names ("zero", "ra", "sp", "a0", "s1", "ft0",
 *     "fa0"; "v8"), or the numeric names with RISCV_FORMAT_NUMERIC ("x10",
 *     "f10"), as objdump -M numeric.
 *   - Immediates in decimal ("-16"): the text of llvm-mc and of llvm-objdump
 *     --no-print-imm-hex (the default of llvm-objdump 23 prints the immediates
 *     in hexadecimal; the tests pass --no-print-imm-hex). An unknown CSR is
 *     its decimal number.
 *   - Memory as "8(sp)", "(a0)", "-4(a1)".
 *   - A branch / jal target as the absolute address in hex ("0x1000", what
 *     objdump prints with the instruction's address applied); the tests
 *     disassemble at the address of the objdump listing.
 *   - The optional operands: ", v0.t" when masked, the rounding mode when
 *     it isn't the default ("fadd.s fa0, fa1, fa2, rtz"); RISCV_FORMAT_NO_ALIASES
 *     prints "dyn" too, the conversions which never round print the mode
 *     only when it isn't rne and print "dyn" in both formats.
 *   - The aliases as objdump prints them by default (li, mv, ret, j, nop,
 *     beqz, csrr, fmv.s, vmv.v.v, vnot.v...), and the compressed
 *     instructions as the 32 bit instruction they expand to ("c.addi a0, 1"
 *     prints "addi a0, a0, 1"). RISCV_FORMAT_NO_ALIASES prints the
 *     instructions without the aliases and the compressed ones by their
 *     "c." name, as objdump -M no-aliases.
 *
 * The exact rules are the ones of LLVM, checked by tests/riscv/run_compat.py.
 */
class RiscvNotation : public OpcodeFormatter {
public:
    /*
     * The format types of StreamDisassembler::getOpcodeFormat(), which can
     * be combined (DEFAULT_FORMATTER, 0, is the default of objdump)
     */
    enum FormatFlags {
        // objdump -M no-aliases
        RISCV_FORMAT_NO_ALIASES = 0x100,
        // objdump -M numeric
        RISCV_FORMAT_NUMERIC    = 0x200
    };

    /*
     * Constructor
     *
     * instruction   - The instruction, a RiscvOpcode (the formatter keeps a
     *                 reference of it, as the other formatters do)
     * dataFormatter - Used for the separator and for endInstruction() only:
     *                 the numbers are formatted in the LLVM syntax
     * formatType    - A combination of FormatFlags
     */
    RiscvNotation(const OpcodePtr& instruction,
                  OpcodeDataFormatter& dataFormatter,
                  int formatType = 0);

    // See OpcodeFormatter
    virtual cString string(OpcodeFormatStruct* formatStruct = NULL) const;
    virtual uint parseOperandAddress(ProcessorAddress& address);

    /*
     * Format one operand
     *
     * operand    - The operand
     * context    - The instruction set and the features (the CSR names)
     * formatType - A combination of FormatFlags
     */
    static cString formatOperand(const RiscvDecodedOperand& operand,
                                 const riscvdis::RiscvCodecContext& context,
                                 int formatType = 0);

    /*
     * Return the name of a register id (RiscvArchitecture.h): the ABI name,
     * or the numeric one with RISCV_FORMAT_NUMERIC
     */
    static const char* getRegisterName(uint registerId, int formatType = 0);

private:
    // The instruction, and its RiscvOpcode view
    OpcodePtr m_data;
    const RiscvOpcode* m_opcode;
    // The GUI formatter
    OpcodeDataFormatter& m_dataFormatter;
    // See FormatFlags
    int m_formatType;
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVNOTATION_H
