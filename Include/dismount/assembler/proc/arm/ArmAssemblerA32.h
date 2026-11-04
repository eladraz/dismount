#ifndef __TBA_DISMOUNT_ASSEMBLER_PROC_ARM_ARMASSEMBLERA32_H
#define __TBA_DISMOUNT_ASSEMBLER_PROC_ARM_ARMASSEMBLERA32_H

/*
 * ArmAssemblerA32.h
 *
 * The AArch32 (A32 and T32) part of the ARM assembler: the syntax of LLVM
 * (the unified syntax) into the bits of one instruction. ArmAssembler.h
 * documents how an instruction is assembled; this file is what is
 * specific to AArch32:
 *
 *   - The operand syntax: registers (r0-r15, sp, lr, pc, the aliases),
 *     s / d / q registers with an element ("d0[1]", "d0[]"), lists ("{r0,
 *     r4-r6, lr}", "{d0, d2}"), immediates ("#4", "#-0x10", "#1.5e0",
 *     "#bits, #rot" of the modified immediate), memory ("[r0, #4]!",
 *     "[r0], r1", "[r0, -r1, lsl #2]", "[r0:128]"), shifts ("lsl #2", "ror
 *     r2", "rrx") and the names (conditions, options, special registers).
 *     The parser produces ArmDecodedOperand (the m_name of every operand is
 *     its text) and the codec encodes it for the spec of an entry.
 *   - T32: the 16 bit encodings are tried before the 32 bit ones (like clang),
 *     the ".w" / ".n" qualifiers choose the size, the floating point and
 *     Advanced SIMD encodings of the A32 table are translated to T32, and the
 *     it block is tracked by the caller (itState).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"

namespace armdis {

/*
 * The result of assembling one AArch32 instruction
 */
struct ArmAssembledA32 {
    // The bits (see ArmEncodingEntry: the T32 hw1 is in the high 16 bits)
    uint32 m_encoding;
    // 2 or 4
    uint m_size;
    // The name a branch refers to, empty if none
    cString m_label;
};

/*
 * Assemble the text of one instruction
 *
 * type     - ARM_A32 or ARM_T32
 * features - The features of the target
 * line     - The text of the instruction (the comments are removed here)
 * address  - The address of the instruction
 * itState  - T32: the it state that applies to the instruction (0 for none)
 * result   - Will be filled
 *
 * Throw exception if the text isn't an instruction of the target
 */
void assembleA32(ArmInstructionSet::DisassemblerTypes type,
                 const ArmFeatureSet& features,
                 const cString& line,
                 uint64 address,
                 uint itState,
                 ArmAssembledA32& result);

/*
 * Return the it state after an instruction (T32): an it instruction starts a
 * block with its firstcond:mask, the other instructions advance it
 */
uint advanceItState(ArmInstructionSet::DisassemblerTypes type,
                    uint itState,
                    const ArmAssembledA32& assembled,
                    uint64 address,
                    const ArmFeatureSet& features);

} // namespace armdis

#endif // __TBA_DISMOUNT_ASSEMBLER_PROC_ARM_ARMASSEMBLERA32_H
