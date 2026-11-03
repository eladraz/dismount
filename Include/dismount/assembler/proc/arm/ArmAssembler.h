#ifndef __TBA_DISMOUNT_ASSEMBLER_PROC_ARM_ARMASSEMBLER_H
#define __TBA_DISMOUNT_ASSEMBLER_PROC_ARM_ARMASSEMBLER_H

/*
 * ArmAssembler.h
 *
 * Assemble the LLVM syntax of the ARM instruction sets (A32, T32 and A64)
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
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmOpcode.h"

/*
 * Implement the AssemblerInterface for the ARM processors.
 */
class ArmAssembler : public AssemblerInterface {
public:
    /*
     * Constructor
     *
     * binary   - The binary to fill the information to
     * type     - The instruction set
     * features - The features of the target: the instructions of the other
     *            features are rejected. The default accepts everything
     */
    ArmAssembler(FirstPassBinaryPtr binary,
                 ArmInstructionSet::DisassemblerTypes type,
                 const ArmFeatureSet& features = ArmFeatureSet::all());

    // See AssemblerInterface::getAssembler()
    virtual cStringerStreamPtr getAssembler();

    // See AssemblerInterface::getFirstPass()
    virtual FirstPassBinaryPtr getFirstPass();

private:
    // The binary object
    FirstPassBinaryPtr m_binary;
    // The instruction set and the features
    ArmInstructionSet::DisassemblerTypes m_type;
    ArmFeatureSet m_features;
};

/*
 * Usage:
 *     ArmAssemblerStream compiler(firstBinaryPass, ArmInstructionSet::ARM_A64);
 *     compiler << "stp x29, x30, [sp, #-16]!" << endl;
 *     compiler << "mov x29, sp" << endl;
 *     compiler << "ldp x29, x30, [sp], #16" << endl;
 *     compiler << "ret" << endl;
 *
 * The syntax is the LLVM one, the text which ArmNotation prints: lowercase
 * mnemonics (the parts from the fields included: "addseq", "vadd.i32",
 * "b.ne"), '#' before the immediates (optional, like LLVM), hexadecimal as
 * 0x.., ';' / '//' / '@' comments. The text of ArmNotation always assembles
 * back to the same bits, except where two encodings have the same text
 * (ARM_ENTRY_DECODE_ONLY).
 *
 * How an instruction is assembled:
 *   1. The line is split into the mnemonic and the operand texts, and each
 *      operand is parsed into an ArmDecodedOperand (by its syntax only).
 *   2. Every entry of the tables of the instruction set (in the order of the
 *      tables) whose mnemonic starts the line's mnemonic, whose mnemonic
 *      parts consume the rest of it (ArmOperandCodec::encodeMnemonicPart)
 *      and whose feature is enabled is a candidate. Its m_value is the start
 *      of the bits; each operand is encoded by ArmOperandCodec::encode().
 *   3. A candidate whose operands all encode is decoded back
 *      (ArmStreamDisassembler::decode): it is accepted only if it gives the
 *      same entry, or an alias whose text is the same. This applies the
 *      constraints of the entries (m_condition, the reserved values) and
 *      the priority of the aliases without writing them twice.
 *   4. T32: the 16 bit encodings are tried first, unless the mnemonic has
 *      the ".w" suffix, and the flag setting rules of the it block decide
 *      between them (adds r0, r1 is 16 bits outside of an it block, add r0,
 *      r1 needs the 32 bit encoding there). The it instruction sets the
 *      conditions which the next instructions must have.
 *
 * A branch to a name is a label: as in IA32AssemblerStream it isn't
 * resolved, the name is added to the dependencies of the binary and the
 * offset is left 0. A branch to "#imm" / ". + imm" is a relative offset.
 *
 * NOTE: This class is not thread-safe
 */
class ArmAssemblerStream : public cStringStream {
public:
    /*
     * Constructor
     *
     * binary   - The binary to fill the information to
     * type     - The instruction set
     * features - The features of the target
     */
    ArmAssemblerStream(FirstPassBinary& binary,
                       ArmInstructionSet::DisassemblerTypes type,
                       const ArmFeatureSet& features = ArmFeatureSet::all());

    /*
     * Destructor. Compile the stream (if any data is pending) and never
     * throws.
     */
    virtual ~ArmAssemblerStream();

    /*
     * Parse and encode all the buffered text into the binary, then clear the
     * buffer. Throws on invalid input. Called by the destructor.
     */
    void assemble();

    /*
     * Assemble one instruction into its bits, without a binary (the tests and
     * dismount_cli use it)
     *
     * type     - The instruction set
     * features - The features of the target
     * line     - The text of one instruction
     * address  - The address of the instruction (for the pc relative
     *            operands)
     * itState  - T32: the it state which applies to the instruction (0 for
     *            none)
     * encoding - Will be filled with the bits (see ArmEncodingEntry)
     * size     - Will be filled with the size, 2 or 4
     *
     * Throw exception if the line isn't a valid instruction of the target
     */
    static void assembleInstruction(ArmInstructionSet::DisassemblerTypes type,
                                    const ArmFeatureSet& features,
                                    const cString& line,
                                    uint64 address,
                                    uint itState,
                                    uint32& encoding,
                                    uint& size);

private:
    // Assemble one line into the binary
    void assembleLine(const cString& line);

    // The binary
    FirstPassBinary& m_binary;
    // The instruction set and the features
    ArmInstructionSet::DisassemblerTypes m_type;
    ArmFeatureSet m_features;
    // T32: the it state of the next instruction
    uint m_itState;
};

#endif // __TBA_DISMOUNT_ASSEMBLER_PROC_ARM_ARMASSEMBLER_H
