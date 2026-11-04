#ifndef __TBA_DISMOUNT_ASSEMBLER_PROC_RISCV_RISCVASSEMBLER_H
#define __TBA_DISMOUNT_ASSEMBLER_PROC_RISCV_RISCVASSEMBLER_H

/*
 * RiscvAssembler.h
 *
 * Assemble the LLVM syntax of the RISC-V instruction sets (RV32 and RV64)
 * into the binary of a FirstPassBinary.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/parser/parser.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"
#include "dismount/proc/riscv/RiscvOpcode.h"

/*
 * Implement the AssemblerInterface for the RISC-V processors.
 */
class RiscvAssembler : public AssemblerInterface {
public:
    /*
     * Constructor
     *
     * binary   - The binary to fill the information to
     * type     - The instruction set
     * features - The features of the target: the instructions of the other
     *            features are rejected. The default accepts everything
     *            (and compresses, the compressed features are in it)
     */
    RiscvAssembler(FirstPassBinaryPtr binary,
                   RiscvInstructionSet::DisassemblerTypes type,
                   const RiscvFeatureSet& features = RiscvFeatureSet::all());

    // See AssemblerInterface::getAssembler()
    virtual cStringerStreamPtr getAssembler();
    // See AssemblerInterface::getFirstPass()
    virtual FirstPassBinaryPtr getFirstPass();

private:
    // The binary object
    FirstPassBinaryPtr m_binary;
    // The instruction set and the features
    RiscvInstructionSet::DisassemblerTypes m_type;
    RiscvFeatureSet m_features;
};

/*
 * Usage:
 *     RiscvAssemblerStream compiler(firstBinaryPass, RiscvInstructionSet::RISCV_RV64);
 *     compiler << "addi sp, sp, -16" << endl;
 *     compiler << "sd ra, 8(sp)" << endl;
 *     compiler << "ld ra, 8(sp)" << endl;
 *     compiler << "addi sp, sp, 16" << endl;
 *     compiler << "ret" << endl;
 *
 * The syntax is the LLVM one, the text which RiscvNotation prints (both the
 * default text and the RISCV_FORMAT_NO_ALIASES / NUMERIC ones): the ABI and the
 * numeric register names (and fp for s0), '#' comments, statements separated by
 * ';'. An immediate is a constant expression as llvm-mc reads them: decimal, 0x
 * hexadecimal, 0b binary and 0 octal numbers, + - * / % << >> & | ^ ~ ! and
 * parentheses, '.' (the address of the instruction). The register names and the
 * mnemonics are case insensitive; the rounding modes, the fence sets and the
 * registers of the compressed instructions are lowercase only, 0X10 is
 * invalid, as in LLVM. The text of RiscvNotation assembles back to the same
 * bits (or to other bits of the same instruction) except for the branch
 * targets: it prints the absolute address (what llvm-objdump prints), the
 * assembler reads the offset from the instruction (see below).
 *
 * How an instruction is assembled:
 *   1. The line is split into the mnemonic and the operand texts, and each
 *      operand is parsed into a RiscvDecodedOperand (by its syntax only).
 *   2. Every entry of the tables (in the order of the tables) whose mnemonic
 *      starts the line's mnemonic, whose mnemonic parts consume the rest of
 *      it (RiscvOperandCodec::encodeMnemonicPart), whose features are
 *      enabled and which exists in the instruction set is a candidate. Its
 *      m_value is the start of the bits; each operand is encoded by
 *      RiscvOperandCodec::encode().
 *   3. A candidate whose operands all encode is decoded back
 *      (RiscvStreamDisassembler::decode): it is accepted only if it gives
 *      the same entry, or an alias whose text is the same. This applies the
 *      constraints of the entries (m_condition, the reserved values) and the
 *      priority of the aliases without writing them twice.
 *   4. Compression: as llvm-mc does, when a compressed feature is enabled
 *      and the 32 bit instruction has a compressed form with the same
 *      operands ("addi a0, a0, 1" is c.addi), the 16 bit encoding is
 *      emitted. A "c." mnemonic always gives the 16 bit encoding. The
 *      compression is turned off by setCompression(false) (".option
 *      norvc" / ".option exact" of llvm-mc).
 *   5. The pseudo instructions of more than one instruction, as llvm-mc
 *      expands them: li of any 32 / 64 bit value (the sequence of LLVM's
 *      RISCVMatInt), la / lla, call / tail, the symbol forms of the loads
 *      and stores ("lw a0, sym").
 *
 * A branch, jal, call, tail or la to a name is a label: as in
 * IA32AssemblerStream it isn't resolved, the name is added to the
 * dependencies of the binary (DEP_RELATIVE: the 4 or 8 bytes of the instructions
 * which hold the reference) and the offset is left 0. A branch to a number
 * is the offset from the instruction, as llvm-mc reads it (to assemble the
 * text of the disassembler, replace the absolute address by the offset or by
 * ". + offset"). A branch to ". + offset" which is out of the range of the
 * branch is relaxed, as llvm-mc does, to the opposite branch over a jal.
 * Without a compressed feature in the set the instructions are never
 * compressed. zext.h, zext.w, sext.b and sext.h are a pair of shifts when
 * their extension isn't in the set.
 *
 * NOTE: This class is not thread-safe
 */
class RiscvAssemblerStream : public cStringStream {
public:
    /*
     * Constructor
     *
     * binary   - The binary to fill the information to
     * type     - The instruction set
     * features - The features of the target
     */
    RiscvAssemblerStream(FirstPassBinary& binary,
                         RiscvInstructionSet::DisassemblerTypes type,
                         const RiscvFeatureSet& features = RiscvFeatureSet::all());

    /*
     * Destructor. Compile the stream (if any data is pending) and never
     * throws.
     */
    virtual ~RiscvAssemblerStream();

    /*
     * Parse and encode all the buffered text into the binary, then clear the
     * buffer. Throws on invalid input. Called by the destructor.
     */
    void assemble();

    /*
     * Turn the compression of step 4 on (the default) or off
     */
    void setCompression(bool isCompressing);

    /*
     * Assemble one instruction into its bits, without a binary (the tests and
     * dismount_cli use it). The pseudo instructions of more than one
     * instruction are rejected: use the stream
     *
     * type          - The instruction set
     * features      - The features of the target
     * line          - The text of one instruction
     * address       - The address of the instruction (for the pc relative
     *                 operands)
     * isCompressing - See step 4
     * encoding      - Will be filled with the bits
     * size          - Will be filled with the size, 2 or 4
     *
     * Throw exception if the line isn't a valid instruction of the target
     */
    static void assembleInstruction(RiscvInstructionSet::DisassemblerTypes type,
                                    const RiscvFeatureSet& features,
                                    const cString& line,
                                    uint64 address,
                                    bool isCompressing,
                                    uint32& encoding,
                                    uint& size);

private:
    // Assemble one line into the binary
    void assembleLine(const cString& line);

    // The binary
    FirstPassBinary& m_binary;
    // The instruction set and the features
    RiscvInstructionSet::DisassemblerTypes m_type;
    RiscvFeatureSet m_features;
    // See setCompression()
    bool m_isCompressing;
};

#endif // __TBA_DISMOUNT_ASSEMBLER_PROC_RISCV_RISCVASSEMBLER_H
