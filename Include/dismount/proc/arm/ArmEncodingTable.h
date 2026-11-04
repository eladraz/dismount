#ifndef __TBA_DISMOUNT_PROC_ARM_ARMENCODINGTABLE_H
#define __TBA_DISMOUNT_PROC_ARM_ARMENCODINGTABLE_H

/*
 * ArmEncodingTable.h
 *
 * The encoding tables of the ARM instruction sets (A32, T32, A64 and their
 * floating point, Advanced SIMD, SVE and SME extensions). The disassembler,
 * the formatter, the assembler and the semantic lifter are all driven by
 * them.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/arm/ArmFeatures.h"

namespace armdis {

/*
 * How an operand is encoded: the kind of the operand and where its fields
 * are. An ArmOperandSpec is the "type" of an operand of an encoding, in the
 * spirit of the IA-32 OperandType, but with the position of its main field:
 * the same kind serves every encoding which has the field at another bit.
 *
 * ArmOperandCodec (ArmOperandCodec.h) turns a spec and the instruction bits
 * into an ArmDecodedOperand, and an ArmDecodedOperand back into the bits.
 * The text is never produced from the bits: the formatter (ArmNotation)
 * prints the ArmDecodedOperand and the assembler parses the text into one,
 * so decoding, formatting, parsing and encoding are inverse functions which
 * the tests check against each other (tests/arm/TEST_PLAN.md).
 *
 * Naming of the kinds:
 *     ARM_KIND_<instruction set>_<what>[_<scheme>]
 * where the instruction set is A64, A32 (also used by T32 when the field is
 * the same), T32 (the 32 bit Thumb), T16 (the 16 bit Thumb), SVE or SME,
 * or none for the kinds which are the same everywhere. To add a kind: add it
 * here with its m_position / m_flags / m_extra meaning, then to the decode
 * and encode switches of ArmOperandCodec.cpp. The formatter and the parser
 * don't change unless the kind produces a new ArmDecodedOperand::Kind.
 */
enum ArmOperandKind {
    // No operand (the end of the list)
    ARM_KIND_NONE,

    // ---- Parts of the mnemonic -------------------------------------------
    // These kinds are formatted into the mnemonic, in their order in the
    // entry, not as operands: "adds" + "eq" = "addseq" (A32 unified syntax:
    // the s comes before the condition), "vadd" + ".i32", "b" + ".eq".
    //
    // The condition, 4 bits at m_position (A32: 28, T16 b<c>: 8, T32 b<c>.w:
    // 22, A64 b.<cond> / bc.<cond>: 0). AL is not printed, except in the A64
    // mnemonics with m_flags ARM_SPEC_PRINT_AL. The A64 mnemonic part is
    // ".eq" (the entry's mnemonic is "b" / "bc"), the AArch32 one "eq".
    // A32 / T32 instructions without this part take their condition from
    // the it block (T32) or are unconditional
    ARM_KIND_CONDITION_MNEMONIC,
    // The S bit at m_position (A32 / T32 bit 20): "s" if set
    ARM_KIND_SETS_FLAGS,
    // The Advanced SIMD data type of AArch32: ".i8", ".s16", ".u32", ".f32",
    // ".p8"... m_extra is an ArmNeonDataTypeScheme (which bits give the size
    // and the signedness), m_position the size field
    ARM_KIND_A32_NEON_DATA_TYPE,
    // A fixed suffix which comes from a 1 bit field: m_position is the bit,
    // m_extra the index of a pair of suffixes in gArmMnemonicSuffixes
    // (ArmOperandCodec.h): ".w" / "" , "b" / "t", "x" / ""...
    ARM_KIND_MNEMONIC_SUFFIX,

    // ---- General purpose registers --------------------------------------
    // A register number of m_extra bits (5 for A64, 4 for A32 / T32, 3 for
    // T16) at m_position. m_flags are ArmGprFlags: the size (W / X, by the
    // sf bit), the meaning of 31 (sp or zr), the pair (casp: an even
    // register and the next one)
    ARM_KIND_GPR,
    // T16: the 4 bit register made of bit m_extra (the top bit, D / N) and
    // the 3 bits at m_position (add r8, r1 / mov / cmp hi registers, bx)
    ARM_KIND_T16_GPR_HIGH,
    // A list of general purpose registers, a 16 bit mask at m_position (A32
    // ldm / stm / push / pop; T32 the same with bit 13 cleared). m_flags
    // ARM_SPEC_LIST_T16 is the 8 bit T16 list with the extra register
    // (lr for push, pc for pop) at bit m_extra
    ARM_KIND_GPR_LIST,

    // ---- Floating point and SIMD registers ------------------------------
    // A64 scalar registers b / h / s / d / q: the number at m_position (5
    // bits), the size from ArmFprSizeScheme m_extra (fixed, or the ftype
    // field, or the size field of the scalar SIMD instructions)
    ARM_KIND_A64_FPR,
    // A64 vector register vN.<T>: the number at m_position, the arrangement
    // (8b, 16b, 4h, 8h, 2s, 4s, 1d, 2d, 1q) from ArmArrangementScheme m_extra
    // (Q bit 30 and a size field, or fixed)
    ARM_KIND_A64_VECTOR,
    // A64 vector element vN.<T>[index]: the number at m_position, the
    // element size and the index from ArmElementScheme m_extra (the H:L:M
    // of the by element instructions, imm5 of dup / ins / umov, imm4...)
    ARM_KIND_A64_VECTOR_ELEMENT,
    // A64 vector list { vN.<T> - vN+k.<T> }: the first register at
    // m_position, the count (1-4) in m_flags bits 0-2 (each opcode of ld1-ld4
    // / ld1r-ld4r / tbl is an entry of its own), the arrangement from
    // m_extra (ArmArrangementScheme), with ARM_SPEC_LIST_ELEMENT
    // (ArmOperandCodec.h) the element form {v0.s, v1.s}[1] of the single
    // structure loads
    ARM_KIND_A64_VECTOR_LIST,
    // AArch32 single precision s register: 4 bits at m_position and the low
    // bit at m_extra (Vd:D)
    ARM_KIND_A32_SREG,
    // AArch32 double precision d register: 4 bits at m_position and the top
    // bit at m_extra (D:Vd). Without FEAT_VFP3 / d32 the top bit must be 0
    ARM_KIND_A32_DREG,
    // AArch32 quad register: D:Vd as for ARM_KIND_A32_DREG, which must be
    // even, the q number is half of it
    ARM_KIND_A32_QREG,
    // AArch32 scalar dN[x] of the by scalar NEON instructions and vmov: the
    // scheme ArmA32ScalarScheme m_extra gives the register and the index
    ARM_KIND_A32_NEON_SCALAR,
    // AArch32 list of consecutive s or d registers: vldm / vstm / vpush /
    // vpop (the first register as ARM_KIND_A32_SREG / DREG at m_position /
    // m_extra, the count from imm8), vld1-4 / vst1-4 (the count and the
    // spacing from the type field, ArmA32NeonListScheme in m_flags)
    ARM_KIND_A32_FP_LIST,

    // ---- SVE and SME registers ------------------------------------------
    // zN.<T>: the number at m_position (5 bits), the element size from
    // ArmSveSizeScheme m_extra (fixed, the size field 22-23, the tsz field
    // of the shifts and dup...). ARM_SVE_NONE prints "zN" alone
    ARM_KIND_SVE_Z,
    // zN.<T>[index]: by element (fmla z0.s, z1.s, z2.s[1]) or dup / mov
    // (z0.s, z1.s[3]), the scheme ArmSveIndexScheme m_extra gives the fields
    // of the index (and of the register for the by element forms, zm at
    // m_position); m_flags bits 0-2 is the element size (0-4: b h s d q,
    // 7: none, "z2[1]") of the operand, except for the schemes which read it
    // from the encoding (ARM_SVE_IDX_DUP)
    ARM_KIND_SVE_Z_INDEXED,
    // A list of z registers: { zN.<T>, zN+1.<T> } or { zN.<T> - zN+3.<T> }
    // (ld2-ld4, tbl, st2-st4, SME2 multi vector). The registers wrap around
    // z31. m_position is the low bit of the encoded register number, m_flags
    // bits 0-2 the count (1-4), bits 4-5 the multiple of the first register
    // (0: 1, 1: 2, 2: 4, 3: 8: the field has fewer bits and the register
    // number is field * multiple, the SME2 multi vector operands), bit 3
    // ARM_SPEC_LIST_STRIDED (SME2 { z0.s, z8.s }). The element size from
    // m_extra (ArmSveSizeScheme). Lists of 3 and 4 are printed as a range
    ARM_KIND_SVE_Z_LIST,
    // pN, pN/m, pN/z, pN.<T>: the number at m_position, m_flags
    // ArmPredicatFlags (3 or 4 bits, the qualifier: none, /m, /z or by a bit
    // of the encoding), the element size from m_extra (ArmSveSizeScheme,
    // ARM_SVE_NONE for none)
    ARM_KIND_SVE_PREDICATE,
    // pnN.<T> of SME2 / SVE2.1, a predicate used as a counter: 3 bits at
    // m_position (pn8-pn15 with ARM_SVE_PRED_HIGH, otherwise 4 bits), the
    // qualifier and the size as for ARM_KIND_SVE_PREDICATE
    ARM_KIND_SVE_PREDICATE_COUNTER,
    // ffr
    ARM_KIND_SVE_FFR,
    // vN.<T> of the quadword reductions (addqv v0.4s, p0, z1.s): the number
    // at m_position, the arrangement 16b 8h 4s 2d from the element size
    // scheme m_extra (ArmSveSizeScheme)
    ARM_KIND_SVE_V128,
    // A register field which is a copy of another field (the Zm of mov z0.d,
    // z1.d = orr z0.d, z1.d, z1.d; the predicates of mov p0.b, p1.b): m_position
    // is the field, m_extra the position of the field it copies, m_flags the
    // width in bits. Not an operand of the text: never decoded (the entry's
    // condition checks the equality), encoded by copying, like
    // ARM_KIND_A64_TIED_REGISTER
    ARM_KIND_SVE_TIED,
    // A general purpose register whose size follows the element size of the
    // instruction: x when the element is a double word (the scheme m_extra,
    // ArmSveSizeScheme, reads the size field), w otherwise (dup z0.s, w1; index
    // z0.d, x1, x2; clasta w0, p0, w0, z1.s). The number at m_position (5
    // bits), m_flags 1: the number 31 is sp / wsp and not the zero register
    ARM_KIND_SVE_GPR,
    // The predicate of psel / pext / pmov, with an element or slice index:
    // pN.<T>[wV, #imm] (psel), pnN[#imm] (pext), the scheme in m_extra
    // (ArmSvePredicateIndexScheme)
    ARM_KIND_SVE_PREDICATE_INDEXED,
    // za tile: za<n>.<T> (fmopa, addha): m_position is the tile number field,
    // m_flags its width and m_extra the element size (ARM_SVE_B ... ARM_SVE_Q)
    ARM_KIND_SME_TILE,
    // za tile slice: za<n><h|v>.<T>[wV, offset{:last}] (mova, ld1w, st1w, the
    // multi slice moves): m_position is the tile and offset field, m_flags
    // ArmSmeSliceFlags (ArmOperandCodec.h)
    ARM_KIND_SME_TILE_SLICE,
    // za array vector: za.<T>[wV, offset{:last}{, vgx2 | vgx4}] (SME2 multi
    // vector), za[wV, offset] (ldr za): m_position is the offset field,
    // m_flags ArmSmeArrayFlags, m_extra the element size
    ARM_KIND_SME_ARRAY,
    // The list of the 64 bit tiles of zero {za0.d, za2.d}: the 8 bit mask at
    // m_position (LLVM writes it with the biggest tiles: {za0.s}, {za})
    ARM_KIND_SME_TILE_LIST,
    // zt0 (the scheme ArmSmeZt0Scheme m_extra: plain, zt0[imm3], zt0[imm2, mul vl])
    ARM_KIND_SME_ZT0,
    // A z register or an unsized list with an element index of SME2:
    // z0.h[1] (by element multiply-add and dot product), z0[1] (the table
    // index of luti), { z0, z1 }[1]. m_position is the register field,
    // m_flags the width of it (bits 0-3) and the element size (bits 4-6: 0-3
    // b h s d, 7 none); m_extra indexes gArmSmeIndexLayouts (generated with
    // the table: the fields of the index, the alignment and the base of the
    // register; a layout with no index is a narrow register: z0-z15)
    ARM_KIND_SME_Z_INDEXED,
    // The memory operand of ldr / str za and zt0: [x8, #5, mul vl] with the
    // offset of the vector select (the scheme ArmSmeMemoryScheme m_extra)
    ARM_KIND_SME_MEMORY,

    // ---- Immediates -----------------------------------------------------
    // An unsigned immediate of m_extra bits at m_position, multiplied by
    // 1 << (m_flags & ARM_SPEC_SCALE_MASK) (the scaled offsets), plus 1 with
    // ARM_SPEC_PLUS_ONE (the widths of bfm / ubfx, the counts of sve). With
    // ARM_SPEC_HEX it's printed in hexadecimal
    ARM_KIND_IMMEDIATE,
    // A signed immediate of m_extra bits at m_position, scaled as above
    ARM_KIND_SIGNED_IMMEDIATE,
    // An immediate made of several fields, concatenated from the most
    // significant: m_extra indexes gArmSplitImmediates (ArmOperandCodec.h),
    // the list of (position, width) of the fields (T32 imm4:i:imm3:imm8 of
    // movw, A32 imm12:imm4 of bkpt, A64 immhi:immlo of adr...)
    ARM_KIND_SPLIT_IMMEDIATE,
    // A64 add / sub immediate: imm12 at 10, the shift sh at 22 (lsl #12)
    ARM_KIND_A64_ARITHMETIC_IMMEDIATE,
    // A64 logical immediate: N:immr:imms at 10-22, the size by m_flags
    // (ARM_GPR_X / ARM_GPR_SF) or the SVE element size (dupm, and / orr /
    // eor immediate of SVE). m_extra 1 prints the value as a signed number
    // of the register size (the mov alias of orr)
    ARM_KIND_A64_LOGICAL_IMMEDIATE,
    // A64 move wide immediate: imm16 at 5, the shift hw at 21 (lsl #16 *
    // hw). The aliases mov (movz / movn / orr) print the value instead
    ARM_KIND_A64_MOVE_WIDE_IMMEDIATE,
    // A64 8 bit floating point immediate (fmov, fmov vector): imm8 at
    // m_position, the size from m_extra. Printed as "#1.50000000"
    ARM_KIND_A64_FLOAT_IMMEDIATE,
    // A64 Advanced SIMD modified immediate (movi / mvni / orr / bic / fmov):
    // a:b:c:d:e:f:g:h at 16-18 and 5-9, cmode at 12, op at 29; with the
    // shift (lsl #8, msl #16) or the expanded 64 bit value
    ARM_KIND_A64_SIMD_MODIFIED_IMMEDIATE,
    // A64 shift amount of the shift by immediate instructions: immh:immb at
    // 16-22, the shift is (esize * 2 - value) for the right shifts
    // (m_flags ARM_SPEC_RIGHT_SHIFT) or (value - esize) for the left shifts
    ARM_KIND_A64_SIMD_SHIFT_IMMEDIATE,
    // A64 fixed point fraction bits of scvtf / fcvtzs: 64 - scale at
    // m_position (6 bits), or the immh:immb of the vector forms
    ARM_KIND_A64_FIXED_POINT_FBITS,
    // AArch32 modified immediate: the rotated 8 bit value of A32 (imm12),
    // or the T32 i:imm3:imm8 (m_flags ARM_SPEC_T32), or the Advanced SIMD
    // a:b:c:d:e:f:g:h with cmode / op (ARM_SPEC_NEON)
    ARM_KIND_A32_MODIFIED_IMMEDIATE,
    // AArch32 floating point immediate of vmov.f32 / f64 (imm4H:imm4L)
    ARM_KIND_A32_FLOAT_IMMEDIATE,
    // SVE immediates: the signed / unsigned 8 bit with the optional lsl #8
    // (add, sub, dup, cpy: sh at 13), the shift of the shift by immediate
    // instructions (tsz:imm3), the floating point 0.5 / 1.0 / 2.0 / 0.0
    // selected by one bit (fadd z0.s, p0/m, z0.s, #0.5), the rotation of the
    // complex instructions (#90, #270), the multiplier "mul #4" of the
    // element count instructions, the 8 bit floating point of fdup. The
    // fields are in the scheme ArmSveImmediateScheme m_extra (the position of
    // a scheme is fixed by the encoding, m_position is not used)
    ARM_KIND_SVE_IMMEDIATE,

    // ---- Shifts and extends ---------------------------------------------
    // A register with a shift: Rm at m_position, the shift type at bits 22-23
    // (A64) / 5-6 (A32), the amount imm6 at 10 (A64) / imm5 at 7 (A32), or
    // the shift register Rs at 8 (A32, ARM_SPEC_REGISTER_SHIFT). m_flags has
    // the ArmGprFlags of the register and ARM_SPEC_NO_ROR when ror is
    // reserved (add / sub). T32: imm3:imm2 at 12 / 6
    ARM_KIND_SHIFTED_REGISTER,
    // A64 extended register: Rm at m_position, the option at 13-15 and the
    // amount imm3 at 10 (uxtb..sxtx, lsl is printed for uxtw / uxtx with sp)
    ARM_KIND_A64_EXTENDED_REGISTER,
    // AArch32 rotation of the extend instructions (sxtb / uxtah...): ror #8
    // / #16 / #24 from the 2 bits at m_position
    ARM_KIND_A32_ROTATION,

    // ---- Memory ---------------------------------------------------------
    // The addressing forms, each one a full [ ... ] operand.
    // A64: [Xn|SP{, #imm}] unsigned scaled imm12 (m_flags scale), [Xn|SP,
    // #simm9]{!} pre index, [Xn|SP], #simm9 post index, [Xn|SP{, #simm7 *
    // scale}] pair (offset, pre, post by the bits 23-24), [Xn, Xm{, extend
    // {#amount}}] register offset, [Xn|SP] base only (exclusives, atomics),
    // [Xn|SP], Xm / #imm post index of the SIMD structures. The form is in
    // ArmA64AddressScheme m_extra, the base at m_position (5)
    ARM_KIND_A64_MEMORY,
    // The literal of ldr / ldrsw / prfm literal and adr / adrp: the address
    // pc + offset (imm19 * 4, the adr immhi:immlo, the adrp page). Decoded
    // as an ArmDecodedOperand::KIND_BRANCH_TARGET (an absolute address)
    ARM_KIND_A64_PC_RELATIVE,
    // AArch32 addressing: [Rn{, #+/-imm12}]{!} / [Rn], #+/-imm12 (P, W, U
    // bits 24, 21, 23), the register offset [Rn, +/-Rm{, shift}], the split
    // imm8 of ldrh / ldrd (imm4H:imm4L), the vldr imm8 * 4, the T16 forms
    // [Rn, #imm5 * size], [Rn, Rm], [sp, #imm8 * 4], [pc, #imm8 * 4], and the
    // T32 forms (imm12 / imm8 with P U W, the register lsl #imm2). The form
    // is ArmA32AddressScheme m_extra, the base at m_position
    ARM_KIND_A32_MEMORY,
    // The literal of ldr / vldr / pld / adr of AArch32: Align(pc, 4) +/-
    // imm, decoded as KIND_BRANCH_TARGET
    ARM_KIND_A32_PC_RELATIVE,
    // The address of the Advanced SIMD element and structure loads of
    // AArch32: [Rn{:align}]{!} or [Rn{:align}], Rm (Rm at 0, 13 means !,
    // 15 none)
    ARM_KIND_A32_NEON_MEMORY,
    // SVE addressing: [Xn|SP{, #imm, mul vl}], [Xn|SP, Xm{, lsl #s}], [Xn|SP,
    // zM.<T>{, extend #s}] (gather / scatter), [zN.<T>{, #imm}] (vector plus
    // immediate). The form is ArmSveAddressScheme m_extra
    ARM_KIND_SVE_MEMORY,

    // ---- Branches -------------------------------------------------------
    // A pc relative branch target: a signed offset of m_extra bits at
    // m_position, scaled by 4 (A64, A32) or 2 (T32 / T16); the scheme
    // ArmBranchScheme in m_flags gives the split fields of T32 (S:J1:J2 of
    // b.w / bl, the i:imm5 of cbz) and the H bit of A32 blx. Decoded as an
    // absolute address (KIND_BRANCH_TARGET), see ArmDecodedOperand
    ARM_KIND_BRANCH_TARGET,

    // ---- Other operands -------------------------------------------------
    // A condition operand (csel / ccmp / fcsel of A64: 4 bits at m_position;
    // the it firstcond)
    ARM_KIND_CONDITION,
    // The mask of it (it, itt, ite...) is a mnemonic part: the x / y / z of
    // the mnemonic from the mask at m_position and firstcond at m_extra
    ARM_KIND_T16_IT_MASK,
    // A64 system register (mrs / msr): o0:op1:CRn:CRm:op2 at 5-19 (the
    // generic s3_1_c2_c3_4 name if it has no name)
    ARM_KIND_A64_SYSTEM_REGISTER,
    // A64 named option of an instruction, m_extra is an ArmOptionScheme:
    // the barrier option of dmb / dsb (CRm), the prefetch operation of prfm
    // (Rt), the pstate field of msr immediate (op1:op2), the operations of
    // sys aliases (dc / ic / at / tlbi: op1:CRn:CRm:op2), bti targets, the
    // SVE predicate pattern (pow2, vl1... all), the SVE prefetch
    // operation...
    ARM_KIND_OPTION,
    // A64 nzcv immediate of ccmp / ccmn / fccmp (4 bits at 0)
    ARM_KIND_A64_NZCV,
    // A64 operand of a bitfield alias, computed from the immr / imms fields
    // of the encoding (immr at 16, imms at 10; the operand size from m_flags,
    // ArmGprFlags): the ArmBitfieldScheme in m_extra
    ARM_KIND_A64_BITFIELD_AMOUNT,
    // A64 register with a left shift by a 3 bit amount at 10 (m_extra: the
    // number of bits, 3): the Rm of addpt / subpt, printed "x2, lsl #1"
    ARM_KIND_A64_LSL_REGISTER,
    // A64 fixed general purpose register operand (chkfeat x16): the x
    // register number m_extra. No field: the encoding has the register
    // implied. Decoded always, encoded only if the operand is that register
    ARM_KIND_A64_FIXED_REGISTER,
    // A64 register which is not an operand of the text: the field at m_position
    // is a copy of the register field at m_extra (the Rm of cinc and ror,
    // which equals Rn). Never decoded into an operand and encoded by copying
    // the field, see ArmOperandCodec::isHidden(). The decoder checks the
    // equality in the m_condition of the entry
    ARM_KIND_A64_TIED_REGISTER,
    // AArch32 named options: the barrier of dmb / dsb / isb (4 bits at 0),
    // the endianness of setend (bit 9), the interrupt flags of cps (aif),
    // the coprocessor fields of mcr / mrc / cdp / ldc (p<n>, c<n>, opc), the
    // special registers of mrs / msr (apsr_nzcvq, cpsr_fc, banked registers),
    // the special registers of vmrs / vmsr (fpscr, fpexc...); m_extra is an
    // ArmOptionScheme
    ARM_KIND_A32_OPTION,

    // ---- A64 SIMD (ArmOperandCodecSimd.cpp) ------------------------------
    // The rotation of fcadd (#90 / #270, one bit at m_position) and of fcmla
    // (#0 / #90 / #180 / #270, two bits at m_position); m_extra is an
    // ArmRotationScheme. Printed in decimal
    ARM_KIND_A64_SIMD_ROTATION,
    // ---- AArch32 additions (A32 and T32), see ArmOperandCodecA32.h --------
    // A fixed operand which has no field: the text is part of the syntax
    // (apsr, spsr, sp, lr, csync, #0...). m_extra is an ArmA32FixedText.
    // Decoded as ArmDecodedOperand::KIND_NAME, which the formatter prints
    ARM_KIND_A32_FIXED,
    // A part of the mnemonic which comes from a field and is not one of the
    // kinds above (the ie / id of cps): the ArmA32MnemonicPart in m_extra
    ARM_KIND_A32_MNEMONIC_PART,
    // An operand made of fields of the encoding which no other kind covers
    // (the 16 bit immediate of bkpt, the bit field operands...): the
    // ArmA32FieldScheme in m_extra, which knows the fields
    ARM_KIND_A32_FIELD,

    // The number of kinds. Must be last
    ARM_KINDS_COUNT
};

/*
 * The general purpose register flags of a spec (ArmOperandSpec::m_flags of
 * ARM_KIND_GPR, ARM_KIND_SHIFTED_REGISTER, ARM_KIND_A64_EXTENDED_REGISTER).
 * Without a size flag the register is the 32 bit register of AArch32.
 */
enum ArmGprFlags {
    // A64: a 64 bit x register
    ARM_GPR_X           = 0x01,
    // A64: a 32 bit w register
    ARM_GPR_W           = 0x02,
    // A64: x if the sf bit (31) is set, w otherwise
    ARM_GPR_SF          = 0x04,
    // A64: number 31 is sp / wsp (otherwise xzr / wzr)
    ARM_GPR_SP          = 0x08,
    // The first register of a pair (casp, ldxp: the register must be even)
    ARM_GPR_PAIR        = 0x10,
    // AArch32: pc (15) is not allowed, the encoding is UNPREDICTABLE (the
    // decoder still decodes it if LLVM does, see the tests)
    ARM_GPR_NO_PC       = 0x20,
    // A64 (the same bit, which A64 doesn't use for NO_PC): the pair of
    // ARM_GPR_PAIR may be the zero register (tlbip xzr, xzr)
    ARM_GPR_PAIR_ZERO   = 0x20,
    // A64: x if bit 22 (the opc of the loads, ldrsb x / w) is clear
    ARM_GPR_OPC22       = 0x40,
    // A64: the register is written back, printed "x2!" (the size operand of
    // cpyfp / setp)
    ARM_GPR_WRITEBACK   = 0x80
};

/*
 * The other flags of a spec, per kind as documented on the kind
 */
enum ArmSpecFlags {
    // ARM_KIND_IMMEDIATE / SIGNED_IMMEDIATE: the scale, a shift count
    ARM_SPEC_SCALE_MASK     = 0x07,
    // ARM_KIND_IMMEDIATE: the value is field + 1
    ARM_SPEC_PLUS_ONE       = 0x08,
    // Print the immediate in hexadecimal
    ARM_SPEC_HEX            = 0x10,
    // ARM_KIND_SHIFTED_REGISTER: the shift amount is a register (A32 Rs)
    ARM_SPEC_REGISTER_SHIFT = 0x20,
    // ARM_KIND_IMMEDIATE / SIGNED_IMMEDIATE: a zero is written "#0" and not
    // "#0x0" (LLVM does it for the exception numbers: svc, brk, hlt...).
    // Shares the bit with ARM_SPEC_REGISTER_SHIFT, which is for registers
    ARM_SPEC_DECIMAL_ZERO   = 0x20,
    // ARM_KIND_SHIFTED_REGISTER: ror is not a valid shift
    ARM_SPEC_NO_ROR         = 0x40,
    // ARM_KIND_CONDITION_MNEMONIC: print al (A64 b.al)
    ARM_SPEC_PRINT_AL       = 0x80,
    // ARM_KIND_CONDITION (m_flags): the operand is the inverted condition
    // (cset, cinc, cneg print the condition which is the inverse of the
    // field). Shares the bit with ARM_SPEC_SCALE_MASK, which a condition
    // doesn't use
    ARM_SPEC_INVERT         = 0x01,
    // ARM_KIND_SVE_Z_LIST (m_flags): the strided list of SME2 / SVE2.1 ({ z0.s,
    // z8.s }, { z1.s, z5.s, z9.s, z13.s }): the stride is 16 / count, the
    // first register is bit m_position + 4 : the low 3 (2 registers) or 2
    // (4 registers) bits at m_position
    ARM_SPEC_LIST_STRIDED   = 0x08
};

/*
 * The flags of an encoding (ArmEncodingEntry::m_flags)
 */
enum ArmEntryFlags {
    // The entry is the preferred disassembly of a more general encoding
    // (mov of orr, cmp of subs, lsl of ubfm, nop of hint #0): the decoder
    // finds it first since it comes first in the table. The assembler accepts
    // both the alias and the general form
    ARM_ENTRY_ALIAS                 = 0x0001,
    // The instruction sets the flags (A64 adds / ands, the T16 instructions
    // which set the flags outside of an it block)
    ARM_ENTRY_SETS_FLAGS            = 0x0002,
    // T16: the instruction sets the flags outside of an it block and not
    // inside one (adds r0, r1, r2 / addeq r0, r1, r2). The decoder adds the
    // "s" to the mnemonic outside of the it block
    ARM_ENTRY_T16_FLAGS_OUTSIDE_IT  = 0x0004,
    // T32: the 32 bit encoding has a 16 bit form, the formatter adds ".w"
    // like LLVM (add.w, ldr.w, b.w)
    ARM_ENTRY_T32_WIDE              = 0x0008,
    // The instruction can't be inside an it block, or must be the last one
    // in it (T32 branches, it itself)
    ARM_ENTRY_T32_NOT_IN_IT         = 0x0010,
    ARM_ENTRY_T32_LAST_IN_IT        = 0x0020,
    // The instruction is UNPREDICTABLE but LLVM decodes it: the decoder
    // decodes it (and the lifter lifts it to OP_UNDEFINED)
    ARM_ENTRY_UNPREDICTABLE         = 0x0040,
    // The assembler doesn't produce this encoding (only the decoder knows
    // it, for example a second encoding of the same text)
    ARM_ENTRY_DECODE_ONLY           = 0x0080,
    // A64 SME: the instruction is valid only in the streaming mode, or is
    // illegal in it (the lifter traps; the decoder decodes both)
    ARM_ENTRY_STREAMING_ONLY        = 0x0100,
    ARM_ENTRY_NOT_STREAMING         = 0x0200,
    // Only the assembler uses the entry: LLVM reads the text but never prints
    // it (cmpp is read for subps xzr, ...: the disassembler prints subps)
    ARM_ENTRY_ASSEMBLE_ONLY         = 0x0400,
    // AArch32: the instruction exists only before ARMv8 (FEAT_V8_AA32 is not
    // in the feature set): swp, the coprocessor instructions of cp other than
    // 14 / 15 and the *2 variants
    ARM_ENTRY_PRE_V8                = 0x0800
};

/*
 * A predicate on the bits of an encoding, for the constraints which a mask
 * can't express: the conditions of the aliases (ubfm is lsl when imms + 1 ==
 * immr), the reserved values of a field (an UNALLOCATED encoding inside the
 * mask).
 *
 * Return true if the encoding is this entry
 */
typedef bool (*ArmEncodingCondition)(uint32 encoding);

/*
 * The spec of one operand of an encoding, see ArmOperandKind
 */
struct ArmOperandSpec {
    // An ArmOperandKind
    uint8 m_kind;
    // The bit of the main field (the low bit)
    uint8 m_position;
    // ArmGprFlags / ArmSpecFlags / per kind, see the kind
    uint8 m_flags;
    // Per kind: the width of the field, a scheme number...
    uint8 m_extra;
};

// The maximum number of operand specs of an encoding (mnemonic parts
// included): mrc<c> p15, #0, r0, c13, c0, #3 has 7
enum { ARM_MAX_OPERAND_SPECS = 8 };

/*
 * An encoding of an instruction.
 *
 * The encoding is a 32 bit value: the A64 and A32 instruction, the T16
 * halfword (in the low 16 bits), or the T32 two halfwords with the first one
 * in the high 16 bits (the bit numbers of the Arm ARM, hw1:hw2).
 *
 * An encoding matches when (bits & m_mask) == m_value, its feature is in
 * the target's ArmFeatureSet and m_condition (if any) returns true. The
 * first matching entry of a table wins, so an alias comes before the
 * general encoding, and a specific encoding before an overlapping general
 * one. The decoder may index a table by some of its bits for speed, but must
 * keep this order.
 *
 * The mnemonic is the LLVM one, lowercase, without the parts which come from
 * fields (the condition, s, the Advanced SIMD data type, see
 * ARM_KIND_CONDITION_MNEMONIC...): "add", "ldr", "vadd", "fmla", "ld1w".
 * The suffixes which are fixed for the encoding are part of it ("ldrb",
 * "vmov.f32" when only one data type exists, "b" for b.cond).
 */
struct ArmEncodingEntry {
    // The mnemonic, or NULL for the end of the table
    const char* m_mnemonic;
    // The fixed bits. m_value is also the start of the assembler: the bits of
    // m_value outside m_mask are the canonical value of the fields that the
    // decoder ignores (the "should be one" bits of ldar, smulh...), which the
    // assembler writes and the decoder doesn't look at
    uint32 m_mask;
    uint32 m_value;
    // The operands and the mnemonic parts, in the order of the syntax,
    // ended by ARM_KIND_NONE
    ArmOperandSpec m_operands[ARM_MAX_OPERAND_SPECS];
    // The ArmFeatures::Feature which adds the encoding
    uint16 m_feature;
    // ArmEntryFlags
    uint16 m_flags;
    // The Opcode::FlowAlter of the instruction (FLOW_NO_ALTER for most)
    uint16 m_flow;
    // An extra constraint, or NULL
    ArmEncodingCondition m_condition;
    // A second feature which the encoding needs too (the instructions which
    // LLVM guards with two features: "instruction requires: sme2
    // sme-i16i64"), 0 (FEAT_A32_BASE, which no A64 entry asks for) for none.
    // The tables leave it out
    uint16 m_feature2;
};

/*
 * The tables, each ended by an entry whose mnemonic is NULL. The
 * disassembler of an instruction set searches its tables in this order.
 */
// A64: the integer, branch, load / store and system instructions, with the
// extensions of them (LSE, PAuth, MTE, MOPS, CSSC...)
extern const ArmEncodingEntry gArm64BaseEncodingTable[];
// A64: the scalar floating point, Advanced SIMD and the cryptographic
// instructions
extern const ArmEncodingEntry gArm64SimdEncodingTable[];
// A64: SVE, SVE2, SVE2.1
extern const ArmEncodingEntry gArm64SveEncodingTable[];
// A64: SME, SME2, SME2.1
extern const ArmEncodingEntry gArm64SmeEncodingTable[];

// A32: the integer, branch, load / store and system instructions
extern const ArmEncodingEntry gArmA32EncodingTable[];
// A32: VFP and Advanced SIMD (and the AArch32 cryptographic instructions).
// T32 reuses it: the T32 floating point and Advanced SIMD encodings are the
// A32 ones with a different top byte (the decoder translates the T32
// encoding into the A32 one before searching this table, and the assembler
// back: 0xF2 / 0xF3 of the A32 data processing become 0xEF / 0xFF, the
// A32 0xF4 element / structure loads become 0xF9, the condition of the A32
// VFP encodings becomes 0xE)
extern const ArmEncodingEntry gArmA32SimdEncodingTable[];
// T32: the 16 bit encodings
extern const ArmEncodingEntry gArmT16EncodingTable[];
// T32: the 32 bit encodings (but the floating point and Advanced SIMD ones)
extern const ArmEncodingEntry gArmT32EncodingTable[];

} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARMENCODINGTABLE_H
