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

} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARMOPERANDCODEC_H
