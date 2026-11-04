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
    // ---- A64 SIMD (ArmOperandCodecSimd.cpp). Where a scheme reads the size
    // field, m_flags & 0xF (if not 0) is the mask of the sizes which are
    // valid: bit n set means size n is allowed
    // Q and imm5 (16-20): the element size is the lowest set bit of imm5
    // (b h s d), 2d needs Q (dup v0.4s, v1.s[1]); imm5 = 10000 is reserved
    ARM_ARR_Q_IMM5,
    // Q and size, with the 1d arrangement (size 3, Q 0) valid: ld1 / st1 / the
    // replicating loads
    ARM_ARR_Q_SIZE_1D,
    // Q: 2s / 4s, and 2h / 4h, and 8b / 16b is ARM_ARR_Q_BYTES, 4h / 8h
    // ARM_ARR_Q_HALF
    ARM_ARR_Q_SINGLE,
    ARM_ARR_Q_2H4H,
    // The sz bit (22) of the long and narrow floating point conversions
    // (fcvtl: 4h / 2s sources, fcvtn: 4h / 2s results, by sz and Q) and the
    // wide side (sz 0: 4s, sz 1: 2d)
    ARM_ARR_Q_SZ_NARROW,
    ARM_ARR_SZ_LONG,
    // immh (19-22) of the shifts, the wide side of the long and narrow
    // shifts: 8h / 4s / 2d for immh 0001 / 001x / 01xx (1xxx is reserved).
    // Q_IMMH with m_flags 1 is the narrow side (1xxx is reserved: 8b 4h 2s)
    ARM_ARR_IMMH_LONG,
    // A fixed arrangement: ArmDecodedOperand::Arrangement in m_flags (a list
    // uses m_flags for its count: ARM_ARR_FIXED_16B above, and 8h here)
    ARM_ARR_FIXED,
    ARM_ARR_FIXED_8H,
    // The element of the single structure loads and stores (ld1 { v0.s }[1]):
    // the element size is the scheme, the index is Q:S:size (b: 4 bits, h: 3,
    // s: 2, d: 1), see ARM_SPEC_LIST_ELEMENT
    ARM_ARR_ELEM_B, ARM_ARR_ELEM_H, ARM_ARR_ELEM_S, ARM_ARR_ELEM_D,
    // The result of the pairwise long instructions (saddlp v0.4h, v1.8b):
    // the elements are twice as wide as the size field and the vector has
    // the same bits (the arrangement of Q and size + 1; size 3 is reserved)
    ARM_ARR_Q_LONG_PAIR,
    ARM_ARR_SCHEMES_COUNT
};

// ARM_KIND_A64_VECTOR m_flags of ARM_ARR_Q_SIZE: bits 0-3 are the mask of the
// valid sizes, and
enum ArmArrangementFlags {
    // 2s (size 2 with Q 0) is reserved: the across lanes instructions
    ARM_ARR_FLAG_NO_2S = 0x10,
    // The size field is at bit 10 (the structure loads and stores) and not
    // at bit 22. Also valid in the m_flags of a vector list, where the low
    // bits are the register count
    ARM_ARR_FLAG_SIZE10 = 0x40
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
    // ---- A64 SIMD (ArmOperandCodecSimd.cpp)
    // The size field (22-23) with m_flags the mask of the valid sizes (bit n:
    // size n): b h s d
    ARM_FPR_SIZE_MASK,
    // 16 << size, m_flags the mask of the valid sizes (h s d: saddlv,
    // sqdmull)
    ARM_FPR_LONG_MASK,
    // The size of the highest set bit of immh (19-22) of the scalar shifts:
    // b h s d; the narrow result of sqshrn (b h s) and the wide source (h s d)
    ARM_FPR_IMMH, ARM_FPR_IMMH_NARROW, ARM_FPR_IMMH_WIDE,
    // The size of the lowest set bit of imm5 (16-20): mov b0, v1.b[1]
    ARM_FPR_IMM5,
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
    // ---- A64 SIMD structure loads and stores (ArmOperandCodecSimd.cpp)
    // [Xn|SP], Xm: the register post index, Rm at 16 (31 is the immediate
    // form)
    ARM_ADDR_SIMD_POST_REG,
    // [Xn|SP], #imm with Rm = 31: the multiple structures, the immediate is
    // the bytes of the transfer, (Q ? 16 : 8) * m_flags (the register count)
    ARM_ADDR_SIMD_POST_IMM_MULTI,
    // The single structure and replicating forms: the immediate is m_flags
    // bytes
    ARM_ADDR_SIMD_POST_IMM_FIXED,
    ARM_ADDR_SCHEMES_COUNT
};

// ARM_KIND_A64_VECTOR_ELEMENT: where the element size and the index are.
// m_position is the register field (Rm: the by element forms, Rn / Rd
// otherwise). H, L, M are the bits 11, 21 and 20
enum ArmElementScheme {
    // imm5 (16-20): the size is the lowest set bit (b h s d), the index is
    // the bits above it. m_flags & 0xF is a mask of the valid sizes (bit n:
    // size n) when not 0 (dup / ins / umov / smov)
    ARM_ELEM_IMM5,
    // The source of ins (element): the size is given by imm5, the index is
    // imm4 (11-14) >> size
    ARM_ELEM_IMM4,
    // The integer by element forms: size 1 (h): index H:L:M and the 4 bit
    // register Rm; size 2 (s): index H:L and a 5 bit Rm; the others are
    // reserved
    ARM_ELEM_BY_SIZE,
    // The floating point by element forms: sz (22) 0: s, index H:L; sz 1: d,
    // index H (L must be 0). A 5 bit Rm
    ARM_ELEM_BY_SZ,
    // h, index H:L:M, a 4 bit Rm (FP16 by element, fmlal by element)
    ARM_ELEM_HALF,
    // 4b, index H:L (sdot by element) and 2h, index H:L (bfdot), a 5 bit Rm
    ARM_ELEM_DOT4B, ARM_ELEM_PAIR2H,
    // fcmla by element: size 1 (h): index H:L; size 2 (s): index H (L = 0);
    // a 5 bit Rm
    ARM_ELEM_FCMLA,
    // 2b, index H:L:M, a 4 bit Rm (fdot by element of FP8)
    ARM_ELEM_PAIR2B,
    // b, index H:L:M:bit 19 and a 3 bit Rm (fmlalb by element of FP8)
    ARM_ELEM_BYTE,
    // d[1] with no field (fmov x0, v1.d[1])
    ARM_ELEM_D1,
    // s, the index in the two bits at 12 (sm3tt1a), a 5 bit Rm at m_position
    ARM_ELEM_IMM2_S,
    // No element size: v2[idx] of luti2 / luti4. The index is the field of
    // m_flags & 3 bits at the position 10 + ((m_flags >> 2) & 7), and the
    // register is a 5 bit field at m_position
    ARM_ELEM_LUT,
    ARM_ELEM_SCHEMES_COUNT
};

// ARM_KIND_A64_SIMD_ROTATION
enum ArmRotationScheme {
    // One bit: #90 or #270 (fcadd)
    ARM_ROT_ADD,
    // Two bits: #0, #90, #180, #270 (fcmla)
    ARM_ROT_MLA,
    ARM_ROT_SCHEMES_COUNT
};

// ARM_KIND_A64_SIMD_MODIFIED_IMMEDIATE: how the imm8 (a:b:c at 16-18,
// d:e:f:g:h at 5-9) and the cmode (12-15) give the value
enum ArmModifiedImmediateScheme {
    // 32 bit elements: lsl 0, 8, 16, 24 by cmode<2:1> (movi, mvni, orr, bic)
    ARM_MODIMM_32_LSL,
    // 16 bit elements: lsl 0, 8 by cmode<1>
    ARM_MODIMM_16_LSL,
    // 32 bit elements with ones shifted in: msl 8, 16 by cmode<0>
    ARM_MODIMM_32_MSL,
    // The 8 bit value (movi v0.8b)
    ARM_MODIMM_8,
    // The 64 bit value, each bit of imm8 a byte (movi d0 / v0.2d)
    ARM_MODIMM_64,
    ARM_MODIMM_SCHEMES_COUNT
};

// ARM_KIND_A64_SIMD_SHIFT_IMMEDIATE m_flags
enum ArmSimdShiftFlags {
    // A right shift: (2 * esize - immh:immb); otherwise a left shift:
    // (immh:immb - esize). esize is the size of the highest set bit of immh,
    // which an earlier operand of the entry set (ARM_ARR_Q_IMMH...)
    ARM_SIMD_SHIFT_RIGHT    = 0x01,
    // The shift is the element size (shll): 8 << size (22-23)
    ARM_SIMD_SHIFT_ESIZE    = 0x02,
    // Printed in hexadecimal ("#0x3")
    ARM_SIMD_SHIFT_HEX      = 0x04,
    // The field is 6 bits at m_position and the element size is 64 (xar)
    ARM_SIMD_SHIFT_XAR      = 0x08
};

// ARM_KIND_A64_FIXED_POINT_FBITS m_flags
enum ArmFixedPointFlags {
    // The vector form: immh:immb at 16, 2 * esize - immh:immb
    ARM_FBITS_VECTOR = 0x01
};

// ARM_KIND_A64_FLOAT_IMMEDIATE m_flags (m_extra: 0 half, 1 single, 2 double)
enum ArmFloatImmediateFlags {
    // imm8 is split as the modified immediate: a:b:c at 16-18, d:e:f:g:h at
    // 5-9 (fmov v0.4s, #1.0); otherwise imm8 is contiguous at m_position
    ARM_FIMM_SPLIT  = 0x01,
    // No field: the operand is #0.0 (fcmp s0, #0.0)
    ARM_FIMM_ZERO   = 0x02,
    // With ARM_FIMM_ZERO: LLVM reads a plain #0 too (fcmeq v0.4s, v1.4s, #0
    // but not fcmp s0, #0); it always prints #0.0
    ARM_FIMM_ZERO_INTEGER = 0x04
};

// ARM_KIND_A64_VECTOR_LIST m_flags
enum ArmSimdListFlags {
    // The count is bits 0-2; the list is the element form { v0.s, v1.s }[1]
    ARM_SPEC_LIST_ELEMENT = 0x80
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
