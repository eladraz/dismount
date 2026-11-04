#ifndef __TBA_DISMOUNT_ASSEMBLER_PROC_RISCV_RISCVASSEMBLERINTERNAL_H
#define __TBA_DISMOUNT_ASSEMBLER_PROC_RISCV_RISCVASSEMBLERINTERNAL_H

/*
 * RiscvAssemblerInternal.h
 *
 * The helpers which RiscvAssembler.cpp and RiscvAssemblerPseudo.cpp share: the
 * parsed operands, one assembled instruction, the pseudo instructions.
 * Not part of the interface.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "xStl/data/string.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvOpcode.h"

namespace riscvasm {

typedef RiscvDecodedOperand Operand;

// What the assembler needs to know about its target
struct Target {
    RiscvInstructionSet::DisassemblerTypes m_type;
    RiscvFeatureSet m_features;
    // The address of the instruction
    uint64 m_address;
    // See RiscvAssemblerStream::setCompression()
    bool m_isCompressing;
};

// One assembled instruction
struct Unit {
    Unit() : m_encoding(0), m_size(0), m_labelSpan(0) {}
    uint32 m_encoding;
    // 2 or 4
    uint m_size;
    // A label which the instruction depends on (the offset is left 0), and
    // the number of bytes from this instruction which hold the reference (a
    // pair of instructions of call / la)
    cString m_label;
    uint m_labelSpan;
};

/*
 * Assemble one instruction from its mnemonic and its operands (no pseudo
 * instruction of more than one instruction). A label in the operands (a
 * KIND_NAME where a branch target is expected) is encoded as 0 and set in
 * 'unit'.
 *
 * Return false if there is no encoding
 */
bool assembleOperands(const Target& target, const cString& mnemonic,
                      const cArray<Operand>& operands, bool allowCompression,
                      Unit& unit);

/*
 * Expand a pseudo instruction of more than one instruction (li of a large
 * value, la, lla, call, tail, the loads and stores of a symbol) into 'units'.
 * Return false if the mnemonic and the operands are not such a pseudo
 * instruction (the caller assembles it as one instruction)
 */
bool expandPseudo(const Target& target, const cString& mnemonic,
                  const cArray<Operand>& operands, cArray<Unit>& units);

// The steps of RISCVMatInt for a value: an instruction mnemonic and its
// immediate. addi / addiw / lui / slli / slli.uw / srli / bseti / bclri /
// rori / sh1add / sh2add / sh3add / add.uw...
struct MaterializeStep {
    const char* m_mnemonic;
    int64 m_immediate;
};
void generateMaterialization(int64 value, uint xlen, const RiscvFeatureSet& features,
                             cArray<MaterializeStep>& steps);

} // namespace riscvasm

#endif // __TBA_DISMOUNT_ASSEMBLER_PROC_RISCV_RISCVASSEMBLERINTERNAL_H
