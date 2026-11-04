#ifndef __TBA_DISMOUNT_PROC_ARM_ARMOPCODE_H
#define __TBA_DISMOUNT_PROC_ARM_ARMOPCODE_H

/*
 * ArmOpcode.h
 *
 * A decoded instruction of A32, T32 or A64, with its operands decoded into
 * structured values (registers, immediates, addresses, lists...).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/array.h"
#include "dismount/Opcode.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/ProcessorAddress.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmArchitecture.h"
#include "dismount/proc/arm/Arm64Architecture.h"

/*
 * A decoded operand of an ARM instruction. The register ids are the ones of
 * ArmArchitecture.h (A32, T32) or Arm64Architecture.h (A64). The fields
 * which a kind doesn't use are 0 / ARM_NO_REGISTER.
 *
 * The formatter prints it (ArmNotation) and the assembler parses the text
 * into it (ArmAssembler), so a kind and its fields are enough to give the
 * text, together with the few printing hints (m_isHex...).
 */
struct ArmDecodedOperand {
    enum Kind {
        // The operand doesn't exist
        KIND_NONE,
        // A register: m_register, with m_arrangement (v0.4s, z0.s, p0.b),
        // m_elementIndex (v0.s[1], d0[1]) and m_predicateQualifier (p0/m)
        KIND_REGISTER,
        // A list of registers: { m_register, m_register + m_stride, ... }
        // m_count registers, with m_arrangement / m_elementIndex (the
        // element of ld1 {v0.s}[1]). The AArch32 general purpose list
        // (ldm, push) is m_registerMask instead
        KIND_REGISTER_LIST,
        // An integer immediate: m_immediate, shifted by m_shift (add #1, lsl
        // #12; movi #1, msl #8)
        KIND_IMMEDIATE,
        // A floating point immediate: m_floatImmediate (fmov #1.5)
        KIND_FLOAT_IMMEDIATE,
        // A register with a shift or an extend: m_register, m_shift (lsl,
        // lsr, asr, ror, rrx, uxtb..sxtx), m_shiftAmount or the register
        // m_shiftRegister (A32 lsl r2)
        KIND_SHIFTED_REGISTER,
        // A memory address, see the m_base... fields
        KIND_MEMORY,
        // A pc relative target (b, bl, cbz, adr, the literal loads): the
        // absolute address m_immediate (the address of the instruction is
        // 0 when the disassembler has no address)
        KIND_BRANCH_TARGET,
        // A condition (csel, ccmp, it): m_condition
        KIND_CONDITION,
        // A system register (mrs / msr): m_systemRegister, the packed
        // o0:op1:CRn:CRm:op2 of A64, or the AArch32 special register number
        // (see ARM_KIND_A32_OPTION)
        KIND_SYSTEM_REGISTER,
        // A named option (dmb ish, prfm pldl1keep, sve vl8, dc civac, bti
        // c, setend be, cps aif, p15 / c13 of mcr): m_option, the value of
        // the field, and m_optionScheme, the ArmOptionScheme (which names)
        KIND_OPTION,
        // An SME za operand: m_register is the first row of the tile
        // (ARM64_REG_ZA_ROW0 for za), m_tile the tile number, m_arrangement
        // the element size; a slice has m_sliceDirection, m_sliceRegister and
        // m_sliceOffset (za0h.s[w12, 1]), an array vector m_vectorGroup
        // (za.d[w8, 0, vgx2]); the tile list of zero has m_registerMask (the
        // 64 bit tiles, bit n is za<n>.d). m_isExplicit is the braces of the
        // slice of ld1b ({za0h.b[w12, 0]}) and of the tile list. zt0 is a
        // KIND_REGISTER of ARM64_REG_ZT0 (m_elementIndex for zt0[8], with
        // SHIFT_MUL_VL for zt0[1, mul vl], m_isExplicit for { zt0 })
        KIND_MATRIX,
        // An identifier the assembler could not classify by its syntax: a
        // condition (ne), an option (ish, pldl1keep), a system register
        // (tpidr_el0) or a label. m_name holds the text, and the codec
        // resolves it for the spec it encodes. The disassembler never
        // produces it
        KIND_NAME
    };

    // The arrangement / element size of a vector register
    enum Arrangement {
        ARRANGEMENT_NONE,
        // A64 Advanced SIMD
        ARRANGEMENT_8B, ARRANGEMENT_16B, ARRANGEMENT_4H, ARRANGEMENT_8H,
        ARRANGEMENT_2S, ARRANGEMENT_4S, ARRANGEMENT_1D, ARRANGEMENT_2D,
        ARRANGEMENT_1Q,
        // The element size only: A64 element (v0.s[1]), SVE z / p (z0.s,
        // p0.b), SME tiles (za0.s), AArch32 has none (the data type is in
        // the mnemonic)
        ARRANGEMENT_B, ARRANGEMENT_H, ARRANGEMENT_S, ARRANGEMENT_D,
        ARRANGEMENT_Q,
        // A64 2h / 4b of the dot product by element (sdot v0.4s, v1.16b,
        // v2.4b[0])
        ARRANGEMENT_2H, ARRANGEMENT_4B,
        // A64 2b of the FP8 dot product by element (fdot v0.4h, v1.8b,
        // v2.2b[3])
        ARRANGEMENT_2B
    };

    // The predicate qualifier
    enum PredicateQualifier {
        PREDICATE_NONE,
        // pN/m
        PREDICATE_MERGING,
        // pN/z
        PREDICATE_ZEROING
    };

    // The shift and extend types
    enum Shift {
        SHIFT_NONE,
        SHIFT_LSL, SHIFT_LSR, SHIFT_ASR, SHIFT_ROR,
        // AArch32 rrx (ror #0)
        SHIFT_RRX,
        // A64 movi / mvni shifting ones
        SHIFT_MSL,
        // A64 extends
        EXTEND_UXTB, EXTEND_UXTH, EXTEND_UXTW, EXTEND_UXTX,
        EXTEND_SXTB, EXTEND_SXTH, EXTEND_SXTW, EXTEND_SXTX,
        // SVE "mul vl" of [x0, #1, mul vl], and the multiplier of cntb / incb
        // (cntb x0, all, mul #4: m_shiftAmount 4)
        SHIFT_MUL_VL,
        SHIFT_MUL
    };

    // The addressing modes of KIND_MEMORY
    enum Addressing {
        // [base, offset]
        ADDRESSING_OFFSET,
        // [base, offset]! the address is written to the base first
        ADDRESSING_PRE_INDEX,
        // [base], offset: the base is the address, then base += offset
        ADDRESSING_POST_INDEX
    };

    // The direction of an SME slice
    enum SliceDirection {
        SLICE_NONE,
        SLICE_HORIZONTAL,
        SLICE_VERTICAL
    };

    // See Kind
    uint m_kind;

    // KIND_REGISTER, KIND_REGISTER_LIST (the first), KIND_SHIFTED_REGISTER:
    // the register id
    uint m_register;
    // See Arrangement
    uint m_arrangement;
    // The element index (v0.s[1], d0[1], z0.s[2]), or -1
    int m_elementIndex;
    // See PredicateQualifier
    uint m_predicateQualifier;
    // The predicate is a counter (pn8)
    bool m_isPredicateCounter;
    // KIND_REGISTER_LIST: the count and the distance between the register
    // numbers (2 for the AArch32 {d0, d2} spaced lists, 8 for the SME2
    // strided {z0.s, z8.s}); m_registerMask for the AArch32 general purpose
    // lists and the za tile list
    uint m_count;
    uint m_stride;
    uint32 m_registerMask;

    // KIND_IMMEDIATE, KIND_BRANCH_TARGET: the value
    int64 m_immediate;
    // KIND_FLOAT_IMMEDIATE: the value
    double m_floatImmediate;

    // KIND_SHIFTED_REGISTER, the index of KIND_MEMORY and the shift of
    // KIND_IMMEDIATE: see Shift, and its amount or register
    uint m_shift;
    uint m_shiftAmount;
    uint m_shiftRegister;

    // KIND_MEMORY: the base register (x / w / sp of A64, r of AArch32, z of
    // the SVE vector plus immediate), the index register or ARM_NO_REGISTER
    // (an x / w / r register, or the z of the SVE gathers with
    // m_indexArrangement), the signed offset of an immediate form, and the
    // addressing mode. The index is shifted / extended by m_shift
    uint m_base;
    uint m_index;
    uint m_indexArrangement;
    int64 m_offset;
    uint m_addressing;
    // KIND_MEMORY: the AArch32 offset is subtracted (the U bit is clear:
    // [r0, -r1], [r0, #-4])
    bool m_isSubtract;
    // KIND_MEMORY: the post index of the A64 SIMD structure loads and the
    // AArch32 NEON loads by a register (ld1 {v0.16b}, [x0], x2); the
    // immediate post index is m_offset
    uint m_postIndexRegister;
    // KIND_MEMORY: the alignment of the AArch32 NEON loads in bits
    // ([r0:128]), 0 for none
    uint m_alignment;

    // KIND_CONDITION: 0 (eq) to 15 (nv)
    uint m_condition;
    // KIND_SYSTEM_REGISTER: see Kind
    uint32 m_systemRegister;
    // KIND_OPTION: see Kind
    uint m_option;
    uint m_optionScheme;

    // KIND_MATRIX: see Kind
    uint m_tile;
    uint m_sliceDirection;
    uint m_sliceRegister;
    uint m_sliceOffset;
    // The last offset of a slice range (za0h.s[w12, 0:1], SME2.1), equal to
    // m_sliceOffset for a single slice
    uint m_sliceOffsetLast;
    // 0, 2 (vgx2) or 4 (vgx4)
    uint m_vectorGroup;

    // KIND_NAME: the identifier, lowercase
    cString m_name;

    // Printing hints: print the immediate in hexadecimal; the operand was
    // written with an explicit "#0" / ", lsl #0" which the formatter keeps
    // (A64 register offset memory: the S bit, the amount is printed). On a
    // KIND_FLOAT_IMMEDIATE it prints "#0.0" (fcmp s0, #0.0) and on an
    // unsigned KIND_IMMEDIATE the 64 bit movi form of LLVM (#0x00000000000001
    // with 14 digits at least, zero as 16 digits without the prefix)
    bool m_isHex;
    bool m_isExplicit;
    // KIND_IMMEDIATE: m_immediate is an unsigned 64 bit value (the hex
    // logical immediates), not a signed one
    bool m_isUnsigned;
    // The assembler only: the immediate was written with a '#', so a branch
    // or an adr takes it as an offset from the instruction and not as an
    // address
    bool m_hasHash;

    // Constructor: KIND_NONE, every field cleared
    ArmDecodedOperand();
};

/*
 * A decoded instruction of an ARM instruction set
 */
class ArmOpcode : public Opcode {
public:
    // The maximum number of operands (the mnemonic parts are not operands)
    enum { MAX_OPERANDS = armdis::ARM_MAX_OPERAND_SPECS };

    // The condition value of an unconditional instruction (AL)
    enum { CONDITION_ALWAYS = 14 };

    /*
     * Constructor, by the disassembler
     *
     * type             - The instruction set
     * shouldUseAddress - Set to true if 'opcodeAddress' is valid
     * opcodeAddress    - The address of the instruction
     * encoding         - The bits, see ArmEncodingEntry
     * size             - 2 (T16) or 4
     * entry            - The encoding of the instruction
     * condition        - The condition of the instruction: the A32 cond
     *                    field, the T32 it block condition, the A64 / T32
     *                    branch condition, or CONDITION_ALWAYS
     * itState          - T32: the it block state, see getItState()
     */
    ArmOpcode(ArmInstructionSet::DisassemblerTypes type,
              bool shouldUseAddress,
              const ProcessorAddress& opcodeAddress,
              uint32 encoding,
              uint size,
              const armdis::ArmEncodingEntry* entry,
              uint condition,
              uint itState);

    // See Opcode
    virtual OpcodeSubsystems::DisassemblerType getType() const;
    virtual cBuffer getOpcode() const;
    virtual uint getOpcodeSize() const;
    virtual bool getOpcodeAddress(ProcessorAddress& address) const;
    virtual bool isBranch() const;
    virtual bool isSwitch() const;
    virtual uint32 getSwitchTableOffset() const;
    virtual int getAlterProperty() const;

    // Return the instruction set
    ArmInstructionSet::DisassemblerTypes getInstructionSet() const;
    // Return the bits of the instruction (see ArmEncodingEntry)
    uint32 getEncoding() const;
    // Return the entry of the encoding table
    const armdis::ArmEncodingEntry* getEntry() const;
    // Return the address of the instruction (0 if the disassembler has no
    // address)
    uint64 getAddress() const;

    /*
     * Return the condition of the instruction, CONDITION_ALWAYS if it is
     * unconditional
     */
    uint getCondition() const;

    /*
     * T32: the it state of the instruction: 0 outside of an it block,
     * otherwise the ITSTATE bits (firstcond:mask) which apply to it. The it
     * instruction itself has the state of the block it starts
     */
    uint getItState() const;
    // T32: true if the instruction is inside an it block, and if it's the
    // last one of the block
    bool isInItBlock() const;
    bool isLastInItBlock() const;

    // Return true if the instruction sets the flags (adds, cmp, the T16
    // instructions outside of an it block, vcmp to fpscr)
    bool isSettingFlags() const;

    // Return the number of operands (not the mnemonic parts)
    uint getOperandsCount() const;
    // Return an operand, 0 is the first. Throw exception if it doesn't exist
    const ArmDecodedOperand& getOperand(uint index) const;
    // Add an operand (the disassembler builds the opcode)
    void addOperand(const ArmDecodedOperand& operand);

    /*
     * Return the full mnemonic, with the parts which come from the fields:
     * "addseq", "vadd.i32", "b.ne", "ldr.w" (built by the disassembler)
     */
    const cString& getMnemonic() const;
    void setMnemonic(const cString& mnemonic);

private:
    // The instruction set
    ArmInstructionSet::DisassemblerTypes m_type;
    // Set to true if m_opcodeAddress is valid
    bool m_shouldUseAddress;
    ProcessorAddress m_opcodeAddress;
    // The bits and the size in bytes
    uint32 m_encoding;
    uint m_size;
    // The encoding
    const armdis::ArmEncodingEntry* m_entry;
    // See getCondition() / getItState()
    uint m_condition;
    uint m_itState;
    // The operands
    ArmDecodedOperand m_operands[MAX_OPERANDS];
    uint m_operandsCount;
    // See getMnemonic()
    cString m_mnemonic;
};

// The reference countable object
typedef cSmartPtr<ArmOpcode> ArmOpcodePtr;

#endif // __TBA_DISMOUNT_PROC_ARM_ARMOPCODE_H
