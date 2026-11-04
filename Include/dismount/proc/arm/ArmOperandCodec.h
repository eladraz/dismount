#ifndef __TBA_DISMOUNT_PROC_ARM_ARMOPERANDCODEC_H
#define __TBA_DISMOUNT_PROC_ARM_ARMOPERANDCODEC_H

/*
 * ArmOperandCodec.h
 *
 * Decode the operands of an ARM encoding from the instruction bits into
 * ArmDecodedOperand, and encode them back. The only place which knows where
 * the fields of an operand are: the disassembler, the assembler and the
 * tests all go through it.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmEncodingTable.h"
#include "dismount/proc/arm/ArmOpcode.h"

namespace armdis {

/*
 * The schemes of the m_extra field of the specs. Each enum lists the ways a
 * kind finds its field; add values as new encodings need them (the codec
 * switches on them). The comment of a value says which fields it reads.
 */

// ARM_KIND_A64_VECTOR / VECTOR_LIST: the arrangement
enum ArmArrangementScheme {
    // Q (30) and size (22-23): 8b 16b 4h 8h 2s 4s 1d(reserved) 2d
    ARM_ARR_Q_SIZE,
    // Q and size, without the byte sizes (size 0 reserved): 4h 8h 2s 4s 2d
    ARM_ARR_Q_SIZE_NO_B,
    // Q and the sz bit (22) of the floating point vectors: 2s 4s 2d
    ARM_ARR_Q_SZ,
    // Q (30) and the half precision vectors: 4h 8h
    ARM_ARR_Q_HALF,
    // The widened destination of the long instructions (saddl: 8h 4s 2d by
    // size) and the narrow source of the high halves (sxtl2...): by size
    ARM_ARR_LONG_SIZE,
    // Q and immh (19-22) of the shift by immediate instructions
    ARM_ARR_Q_IMMH,
    // Fixed arrangements (the crypto 16b / 4s, pmull 1q, tbl 8b / 16b by Q)
    ARM_ARR_FIXED_16B, ARM_ARR_FIXED_4S, ARM_ARR_FIXED_2D, ARM_ARR_FIXED_1Q,
    ARM_ARR_Q_BYTES,
    ARM_ARR_SCHEMES_COUNT
};

// ARM_KIND_A64_FPR: the size of a scalar register
enum ArmFprSizeScheme {
    // Fixed b, h, s, d, q
    ARM_FPR_B, ARM_FPR_H, ARM_FPR_S, ARM_FPR_D, ARM_FPR_Q,
    // The ftype field (22-23) of the floating point instructions: s d (q)
    // h
    ARM_FPR_FTYPE,
    // The size field (22-23) of the scalar Advanced SIMD instructions: b h
    // s d
    ARM_FPR_SIZE,
    // The sz bit (22) of the scalar floating point SIMD instructions: s d
    ARM_FPR_SZ,
    // The opc:size of the SIMD loads and stores (ldr b / h / s / d / q)
    ARM_FPR_LOAD_STORE,
    // The widened size of the long scalar instructions (sqdmull s / d)
    ARM_FPR_LONG_SIZE,
    ARM_FPR_SCHEMES_COUNT
};

// ARM_KIND_SVE_Z / Z_LIST / PREDICATE / SME_TILE: the element size
enum ArmSveSizeScheme {
    // No size (p0 without a suffix)
    ARM_SVE_NONE,
    // Fixed b, h, s, d, q
    ARM_SVE_B, ARM_SVE_H, ARM_SVE_S, ARM_SVE_D, ARM_SVE_Q,
    // The size field 22-23: b h s d
    ARM_SVE_SIZE_22,
    // The size field, the wider element of the widening instructions (h s d
    // by size 1-3) and the narrower element (b h s)
    ARM_SVE_SIZE_22_WIDE,
    ARM_SVE_SIZE_22_NARROW,
    // tsz (22-23 : 8-9) of the shifts, and imm2:tsz of dup / index
    ARM_SVE_TSZ,
    ARM_SVE_DUP_TSZ,
    // The sz bit 22 of the floating point instructions (s d) and the size
    // 22-23 without b (h s d)
    ARM_SVE_SZ_22,
    ARM_SVE_SIZE_22_FLOAT,
    // The dtype of the contiguous loads (ld1b / ld1sb ... into .h .s .d)
    ARM_SVE_LOAD_DTYPE,
    ARM_SVE_SIZE_SCHEMES_COUNT
};

// ARM_KIND_A64_MEMORY: the addressing form. The base register is at m_position
// (5). m_flags & ARM_SPEC_SCALE_MASK is the shift of the offset (the access
// size), and the bits of ARM_ADDR_FLAG_* of m_flags (above the scale) refine
// the form
enum ArmA64AddressScheme {
    // [Xn|SP{, #imm12 << scale}]: the unsigned offset, imm12 at 10
    ARM_ADDR_UNSIGNED_OFFSET,
    // [Xn|SP{, #simm9}]: the unscaled offset of ldur / ldtr, simm9 at 12
    // (shifted by the scale, which is 0 except for the memory tagging
    // instructions where it is 4)
    ARM_ADDR_UNSCALED,
    // [Xn|SP, #simm9]! and [Xn|SP], #simm9: simm9 at 12
    ARM_ADDR_PRE_INDEX,
    ARM_ADDR_POST_INDEX,
    // [Xn|SP{, #simm7 << scale}], with ! and the post index: simm7 at 15
    ARM_ADDR_PAIR_OFFSET,
    ARM_ADDR_PAIR_PRE_INDEX,
    ARM_ADDR_PAIR_POST_INDEX,
    // [Xn|SP, Xm|Wm{, extend {#amount}}]: Rm at 16, option at 13, S at 12;
    // the amount is the scale when S is set
    ARM_ADDR_REGISTER_OFFSET,
    // [Xn|SP]
    ARM_ADDR_BASE,
    // [Xn]! of the memory copy and set instructions
    ARM_ADDR_BASE_WRITEBACK,
    // [Xn|SP, #simm10 << 3] and [Xn|SP, #simm10 << 3]! of ldraa / ldrab
    // (the S bit 22 is the sign, imm9 at 12)
    ARM_ADDR_PAUTH_OFFSET,
    ARM_ADDR_PAUTH_PRE_INDEX,
    // [Xn|SP, #-size]! and [Xn|SP], #size of the ldapr / stlr post and pre
    // index (FEAT_LRCPC3): the size is 1 << scale
    ARM_ADDR_FIXED_PRE_INDEX,
    ARM_ADDR_FIXED_POST_INDEX,
    ARM_ADDR_SCHEMES_COUNT
};

// ARM_KIND_A64_BITFIELD_AMOUNT: how the operand comes from immr / imms
enum ArmBitfieldScheme {
    // lsl Rd, Rn, #n: n = size - 1 - imms (immr = -n mod size)
    ARM_BF_LSL_AMOUNT,
    // ubfx / sbfx / bfxil width: imms - immr + 1
    ARM_BF_WIDTH,
    // ubfiz / sbfiz / bfi / bfc lsb: (-immr) mod size
    ARM_BF_LSB_NEGATED,
    ARM_BF_SCHEMES_COUNT
};

// ARM_KIND_A64_PC_RELATIVE: which fields give the target
enum ArmPcRelativeScheme {
    // adr: immhi:immlo (bits 5-23 : 29-30), a byte offset
    ARM_PCREL_ADR,
    // adrp: immhi:immlo pages, relative to the page of the instruction
    ARM_PCREL_ADRP,
    // The literal loads: imm19 at 5, scaled by 4
    ARM_PCREL_LITERAL,
    // retaasppc / autiasppc: the address minus imm16 * 4, imm16 at 5
    ARM_PCREL_NEGATIVE16,
    ARM_PCREL_SCHEMES_COUNT
};

// ARM_KIND_A64_MOVE_WIDE_IMMEDIATE: how the operand is printed
enum ArmMoveWideScheme {
    // movz / movn / movk x0, #imm16{, lsl #16 * hw}
    ARM_MOVE_WIDE_PLAIN,
    // The mov alias of movz: the value imm16 << shift
    ARM_MOVE_WIDE_MOVZ,
    // The mov alias of movn: the value ~(imm16 << shift)
    ARM_MOVE_WIDE_MOVN,
    ARM_MOVE_WIDE_SCHEMES_COUNT
};

// ARM_KIND_OPTION: the named values and where the field is. m_position and
// m_flags of the spec are not used, the scheme knows its fields
enum ArmOptionScheme {
    // dmb / dsb: CRm (8, 4 bits), the names oshld... sy
    ARM_OPT_BARRIER,
    // dsb <option>nXS: CRm bits 10-11 (bit 8-9 are 0 / 1): oshnxs...
    ARM_OPT_BARRIER_NXS,
    // isb: CRm, "sy" (15) is the default
    ARM_OPT_ISB,
    // prfm: Rt (0, 5 bits): pldl1keep... plil1keep.. pstl3strm
    ARM_OPT_PREFETCH,
    // msr <pstatefield>, #imm: op1 (16, 3 bits) : op2 (5, 3 bits)
    ARM_OPT_PSTATE,
    // The sys aliases: op1 (16) CRn (12) CRm (8) op2 (5) as 14 bits
    ARM_OPT_SYS_DC,
    ARM_OPT_SYS_IC,
    ARM_OPT_SYS_AT,
    ARM_OPT_SYS_TLBI,
    ARM_OPT_SYS_PLBI,
    ARM_OPT_SYS_BRB,
    ARM_OPT_SYS_PREDRES,
    // bti: bits 6-7 give the target: (none), c, j, jc
    ARM_OPT_BTI,
    // The fixed operand "csync" of psb / tsb
    ARM_OPT_CSYNC,
    // The fixed operand "sm", "za" of smstart / smstop (op2 bits)
    ARM_OPT_SVCR,
    // stshh: op2 (5, 3 bits), keep / strm
    ARM_OPT_STSHH,
    // The fixed operand "dsync" of gcsb
    ARM_OPT_DSYNC,
    // The coprocessor style register c0-c15 of sys / sysl: 4 bits at the
    // m_position of the spec
    ARM_OPT_CREG,
    // tlbip: the operation as for the other sys aliases (op1:CRn:CRm:op2)
    ARM_OPT_SYS_TLBIP,
    // rprfm: bit 15, bit 13, bit 12 and Rt<2:0> as a 6 bit prefetch operation
    ARM_OPT_RPRFM,
    // The fixed operand "nb" of tchangef, tchangeb and texit
    ARM_OPT_NB,
    ARM_OPT_SCHEMES_COUNT
};

// ARM_KIND_BRANCH_TARGET: the m_flags values
enum ArmBranchScheme {
    // A plain signed field
    ARM_BRANCH_PLAIN
};

// ARM_KIND_SPLIT_IMMEDIATE: the fields of an immediate, from the most
// significant. m_extra indexes gArmSplitImmediates
struct ArmSplitImmediate {
    // The number of fields (up to 4)
    uint8 m_count;
    // The (position, width) of each field
    uint8 m_position[4];
    uint8 m_width[4];
};
extern const ArmSplitImmediate gArmSplitImmediates[];
enum {
    // tbz / tbnz bit number: b5 (31) : b40 (19, 5 bits)
    ARM_SPLIT_TBZ_BIT = 0,
    ARM_SPLIT_COUNT
};

// ARM_KIND_MNEMONIC_SUFFIX: pairs of suffixes (clear bit, set bit)
struct ArmMnemonicSuffix {
    const char* m_clear;
    const char* m_set;
};
extern const ArmMnemonicSuffix gArmMnemonicSuffixes[];
enum {
    // ".w" is the T32 wide marker, nothing for the narrow
    ARM_SUFFIX_WIDE_NARROW = 0,
    ARM_SUFFIX_COUNT
};

/*
 * The context of decoding or encoding an operand
 */
struct ArmCodecContext {
    // The instruction set
    ArmInstructionSet::DisassemblerTypes m_type;
    // The address of the instruction (for the pc relative operands), 0 if
    // the disassembler has no address
    uint64 m_address;
    // The features of the target (FEAT_VFP3 allows d16-d31)
    ArmFeatureSet m_features;
    // The encoding being decoded or encoded
    const ArmEncodingEntry* m_entry;
};

/*
 * Decode and encode the operands of an encoding
 */
class ArmOperandCodec {
public:
    /*
     * Decode an operand
     *
     * spec     - The spec of the operand (not a mnemonic part)
     * encoding - The instruction bits
     * context  - See ArmCodecContext
     * operand  - Will be filled with the operand
     *
     * Return false if the field has a reserved value: the encoding is not
     * this entry (the decoder tries the next one)
     */
    static bool decode(const ArmOperandSpec& spec,
                       uint32 encoding,
                       const ArmCodecContext& context,
                       ArmDecodedOperand& operand);

    /*
     * Encode an operand: set the fields of 'spec' in 'encoding' (the other
     * bits are kept)
     *
     * Return false if the operand can't be encoded by this spec (another
     * kind of operand, a register of another class or size, an immediate out
     * of range or not a multiple of the scale...). The assembler tries the
     * next entry of the mnemonic
     */
    static bool encode(const ArmOperandSpec& spec,
                       const ArmDecodedOperand& operand,
                       const ArmCodecContext& context,
                       uint32& encoding);

    /*
     * Return true if a spec is a mnemonic part (ARM_KIND_CONDITION_MNEMONIC,
     * ARM_KIND_SETS_FLAGS, ARM_KIND_A32_NEON_DATA_TYPE,
     * ARM_KIND_MNEMONIC_SUFFIX, ARM_KIND_T16_IT_MASK) and not an operand
     */
    static bool isMnemonicPart(const ArmOperandSpec& spec);

    /*
     * Return the text of a mnemonic part for the bits ("eq", "s", ".i32",
     * ".w", "tte"), "" if it prints nothing (al)
     */
    static cString decodeMnemonicPart(const ArmOperandSpec& spec,
                                      uint32 encoding,
                                      const ArmCodecContext& context);

    /*
     * Encode a mnemonic part from the text which follows the base mnemonic:
     * consume its text from the start of 'suffix' (the condition "eq" of
     * "addeq", the ".i32" of "vadd.i32") and set its field
     *
     * Return false if the text doesn't start with a valid value of the part
     * (an empty value is valid for the optional parts: al, no s)
     */
    static bool encodeMnemonicPart(const ArmOperandSpec& spec,
                                   cString& suffix,
                                   const ArmCodecContext& context,
                                   uint32& encoding);

    /*
     * Return the bits of a field / set the bits of a field
     */
    static uint32 getField(uint32 encoding, uint position, uint width);
    static void setField(uint32& encoding, uint position, uint width, uint32 value);

    /*
     * Return true if a spec is hidden: it has no text (ARM_KIND_A64_TIED_
     * REGISTER). The decoder skips it, the assembler calls encode() with an
     * empty operand
     */
    static bool isHidden(const ArmOperandSpec& spec);
};

/*
 * The names of the registers for the formatter and the parser: the names of
 * ArmArchitecture / Arm64Architecture ("r0", "sp", "x0", "w30", "wzr", "v0",
 * "q0", "z31", "p15"...). The AArch32 aliases (r13 / sp, r14 / lr, r15 / pc,
 * r9 / sb, r10 / sl, r11 / fp, r12 / ip) and the A64 ones (x29 / fp, x30 /
 * lr) are accepted by findRegister(); getRegisterName() gives the LLVM
 * printed name.
 */
const char* getRegisterName(ArmInstructionSet::DisassemblerTypes type, uint registerId);
bool findRegister(ArmInstructionSet::DisassemblerTypes type,
                  const cString& name,
                  uint& registerId);

/*
 * The names of the values of a field (ArmOptionScheme), of the system
 * registers and of the conditions, for the formatter and the parser.
 *
 * getOptionName returns NULL when the value has no name (the formatter
 * prints "#0x..." then). findOption looks a name up (case insensitive, the
 * generic "#imm" is the parser's business). The tables are the names of the
 * LLVM assembler, generated by tests/arm/tools/gen_a64_names.py.
 */
const char* getOptionName(uint scheme, uint value);
bool findOption(uint scheme, const cString& name, uint& value);

/*
 * The A64 system registers: 'packed' is op0:op1:CRn:CRm:op2 (16 bits, the
 * instruction bits 5-20; the encodings with op0 below 2 are not system
 * registers, but LLVM names them generically). getSystemRegisterName returns
 * NULL if the register has no name, or isn't readable (mrs) / writable (msr)
 * in the LLVM table: the formatter prints the generic name
 * S<op0>_<op1>_C<n>_C<m>_<op2> then, as LLVM does. findSystemRegister also
 * accepts that generic form, in any case.
 */
const char* getSystemRegisterName(uint32 packed, bool isWrite);
bool findSystemRegister(const cString& name, bool isWrite, uint32& packed);

/*
 * The name of a condition code, 0 (eq) to 15 (nv): "eq", "ne", "hs", "lo",
 * "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv".
 * findCondition also accepts "cs" and "cc"
 */
const char* getConditionName(uint condition);
bool findCondition(const cString& name, uint& condition);

} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARMOPERANDCODEC_H
