#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVOPERANDCODEC_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVOPERANDCODEC_H

/*
 * RiscvOperandCodec.h
 *
 * Decode the operands of a RISC-V encoding into RiscvDecodedOperand values,
 * and encode them back, by the RiscvOperandSpec of the encoding table. The
 * schemes of the immediates and the tables of names (CSRs, the fli values)
 * are here.
 *
 *   RiscvOperandCodec.cpp          the scalar kinds, the CSR names
 *   RiscvOperandCodecCompressed.cpp the compressed schemes, the expansion
 *   RiscvOperandCodecVector.cpp    the vector kinds (vtype, the mask)
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"
#include "dismount/proc/riscv/RiscvFeatures.h"
#include "dismount/proc/riscv/RiscvEncodingTable.h"
#include "dismount/proc/riscv/RiscvOpcode.h"

namespace riscvdis {

/*
 * The schemes of the immediates (RISCV_KIND_IMMEDIATE, the offsets of
 * RISCV_KIND_MEMORY and RISCV_KIND_BRANCH_TARGET): which bits of the
 * instruction give which bits of the value, the sign and the scale. The
 * names are the formats of the specification. Add the schemes the tables
 * need here (the decode and the encode of each one are a pair of
 * functions of RiscvOperandCodec*.cpp).
 */
enum RiscvImmediateScheme {
    // --- 32 bit formats
    // I: imm[11:0] = inst[31:20], signed
    RISCV_IMM_I,
    // S: imm[11:5] = inst[31:25], imm[4:0] = inst[11:7], signed
    RISCV_IMM_S,
    // B: imm[12|10:5] = inst[31:25], imm[4:1|11] = inst[11:7], signed, even
    RISCV_IMM_B,
    // U: imm[31:12] = inst[31:12], the 20 bit value as LLVM prints it (lui /
    // auipc a0, 0x12345 is the 20 bit field)
    RISCV_IMM_U,
    // J: imm[20|10:1|11|19:12] = inst[31:12], signed, even
    RISCV_IMM_J,
    // The unsigned 5 bit immediate of the CSR instructions (inst[19:15])
    RISCV_IMM_CSR_UIMM5,
    // The offset of prefetch.i / r / w: I with imm[4:0] = 0 (inst[24:20] is
    // part of the encoding)
    RISCV_IMM_PREFETCH,
    // The aes32* byte select (inst[31:30]) and the aes64ks1i round number
    // (inst[23:20], 0-10)
    RISCV_IMM_AES_BS,
    RISCV_IMM_AES_RNUM,
    // The lpad label: inst[31:12], unsigned 20 bits
    RISCV_IMM_LPAD,

    // --- Vector
    // simm5 / uimm5 of the .vi instructions (inst[19:15])
    RISCV_IMM_V_SIMM5,
    RISCV_IMM_V_UIMM5,
    // The 6 bit uimm of vror.vi (inst[26] and inst[19:15])
    RISCV_IMM_V_UIMM6,
    // The uimm5 of vsetivli (avl, inst[19:15])
    RISCV_IMM_V_AVL,

    // --- Compressed formats (the immediates of the "c." syntax)
    // CI: imm[5] = inst[12], imm[4:0] = inst[6:2], signed (c.addi, c.li,
    // c.andi of CB, c.addiw)
    RISCV_IMM_CI,
    // c.lui: nzimm[17] = inst[12], nzimm[16:12] = inst[6:2], printed as the
    // 20 bit value of lui (a negative one is 0xfffe0..0xfffff), non zero
    RISCV_IMM_CI_LUI,
    // c.addi16sp: nzimm[9|4|6|8:7|5], scaled by 16, signed, non zero
    RISCV_IMM_CI_ADDI16SP,
    // c.addi4spn: nzuimm[5:4|9:6|2|3] = inst[12:5], scaled by 4, non zero
    RISCV_IMM_CIW,
    // c.lwsp / c.flwsp: uimm[5|4:2|7:6], scaled by 4; c.ldsp / c.fldsp:
    // uimm[5|4:3|8:6], scaled by 8
    RISCV_IMM_CI_LWSP,
    RISCV_IMM_CI_LDSP,
    // c.swsp / c.fswsp: uimm[5:2|7:6] = inst[12:7]; c.sdsp / c.fsdsp:
    // uimm[5:3|8:6]
    RISCV_IMM_CSS_SWSP,
    RISCV_IMM_CSS_SDSP,
    // c.lw / c.sw / c.flw / c.fsw: uimm[5:3|2|6]; c.ld / c.sd / c.fld /
    // c.fsd: uimm[5:3|7:6]
    RISCV_IMM_CL_W,
    RISCV_IMM_CL_D,
    // c.lbu / c.sb: uimm[0|1] = inst[6:5]; c.lhu / c.lh / c.sh: uimm[1] =
    // inst[5]
    RISCV_IMM_CLB,
    RISCV_IMM_CLH,
    // c.beqz / c.bnez: offset[8|4:3] = inst[12:10], offset[7:6|2:1|5] =
    // inst[6:2], signed, even
    RISCV_IMM_CB,
    // c.j / c.jal: offset[11|4|9:8|10|6|7|3:1|5] = inst[12:2], signed, even
    RISCV_IMM_CJ
};

// The flags of RISCV_KIND_IMMEDIATE
enum RiscvImmediateFlags {
    // 0 is reserved for this entry (the decoder doesn't match it)
    RISCV_IMMEDIATE_NOT_ZERO    = 0x01,
    // Printed in hexadecimal
    RISCV_IMMEDIATE_HEX         = 0x02
};

// The flags of RISCV_KIND_MEMORY
enum RiscvMemoryFlags {
    // The base is a compressed register (3 bits at m_position)
    RISCV_MEMORY_COMPRESSED     = 0x01,
    // The base is sp (c.lwsp, c.swsp...), no bits
    RISCV_MEMORY_SP             = 0x02,
    // "(base)" without an offset: the atomics, lr / sc, cbo.*, the vector
    // loads and stores (the base is 5 bits at m_position, m_extra unused)
    RISCV_MEMORY_NO_OFFSET      = 0x04
};

// The flags of RISCV_KIND_ROUNDING_MODE
enum RiscvRoundingFlags {
    // The default which isn't printed is rne instead of dyn: the
    // conversions which never round (fcvt.d.s, fcvt.d.w, fcvt.s.h...), for
    // which LLVM uses rne as the default (and prints dyn when it's dyn)
    RISCV_ROUNDING_DEFAULT_RNE  = 0x01
};

// The flags of RISCV_KIND_VR, on the destination spec: the constraints LLVM
// checks when it assembles (the decoder doesn't check them, LLVM decodes the
// reserved overlaps). The generator probes them from llvm-mc for each entry
// (tests/riscv/tools/gen_encoding_tables.py); RiscvOperandCodec::
// checkAssemblerConstraints applies them
enum RiscvVectorRegisterFlags {
    // The destination of a masked instruction (vm = 0) can't be v0
    RISCV_VR_NOT_MASK_WHEN_MASKED   = 0x01,
    // The destination can't be the register of vs2 / of vs1 (the widening
    // instructions, vrgather, vslideup, vcompress, viota...). A segment tuple
    // destination (m_extra of the spec) overlaps a source which is any of its
    // registers
    RISCV_VR_NO_VS2_OVERLAP         = 0x02,
    RISCV_VR_NO_VS1_OVERLAP         = 0x04,
    // The destination can't be v0 whether or not it is masked (vadc, vsbc,
    // vmerge, vfmerge)
    RISCV_VR_NOT_V0                 = 0x08,
    // Both of the sources
    RISCV_VR_NO_SOURCE_OVERLAP      = RISCV_VR_NO_VS2_OVERLAP | RISCV_VR_NO_VS1_OVERLAP
};

/*
 * The values of the fli immediate (Zfa): 32 entries, the index is the rs1
 * field. The text is LLVM's ("min", "inf", "nan", "1.0", "0.5"...), the
 * bits are the single / double / half values (min is the smallest normal of
 * each format)
 */
struct RiscvFliValue {
    const char* m_text;
    uint16 m_half;
    uint32 m_single;
    uint64 m_double;
};
extern const RiscvFliValue gRiscvFliValues[32];

/*
 * The names of the CSRs which LLVM 23 prints (RiscvCsrNames.cpp, generated from
 * llvm-objdump: the unprivileged ones, the supervisor, hypervisor and machine
 * ones), ended by a NULL name. LLVM 23 prints and reads a name whichever
 * features are enabled, so every entry is FEAT_I; the RV32 only ones are the
 * h halves. A CSR may have more than one name: the first one is the one
 * printed, the others are accepted by the assembler (RISCV_CSR_ALTERNATIVE:
 * sbadaddr, mbadaddr, sptbr, dscratch)
 */
struct RiscvCsrName {
    const char* m_name;
    // The CSR number
    uint16 m_number;
    // The RiscvFeatures::Feature which adds it (FEAT_I for always)
    uint16 m_feature;
    // RISCV_CSR_*
    uint16 m_flags;
};
enum RiscvCsrFlags {
    // The CSR exists in RV32 only (cycleh, the h halves)
    RISCV_CSR_RV32_ONLY         = 0x01,
    // An alternative name, accepted only
    RISCV_CSR_ALTERNATIVE       = 0x02
};
extern const RiscvCsrName gRiscvCsrNames[];

/*
 * What the codec needs besides the spec and the bits
 */
struct RiscvCodecContext {
    // The instruction set (XLEN)
    RiscvInstructionSet::DisassemblerTypes m_type;
    // The features of the target (the CSR names)
    RiscvFeatureSet m_features;
    // The address of the instruction (the pc relative operands)
    uint64 m_address;
};

} // namespace riscvdis

/*
 * The codec of the operands
 */
class RiscvOperandCodec {
public:
    /*
     * Decode an operand
     *
     * spec     - The spec of the operand (not a mnemonic part)
     * encoding - The instruction bits
     * context  - See RiscvCodecContext
     * operand  - Will be filled with the operand
     *
     * Return false if the field has a reserved value: the encoding is not
     * this entry (the decoder tries the next one). A hidden spec
     * (RISCV_KIND_GPR_TIED) has no operand: it returns whether the two fields
     * are equal. The optional specs (rounding mode, mask) are always decoded;
     * isPrinted() tells whether the text has them
     */
    static bool decode(const riscvdis::RiscvOperandSpec& spec,
                       uint32 encoding,
                       const riscvdis::RiscvCodecContext& context,
                       RiscvDecodedOperand& operand);

    /*
     * Encode an operand: set the fields of 'spec' in 'encoding' (the other
     * bits are kept)
     *
     * Return false if the operand can't be encoded by this spec (another
     * kind of operand, a register of another class or a compressed
     * register out of x8-x15, an immediate out of range or not a multiple of
     * the scale...). The assembler tries the next entry of the mnemonic. For an
     * optional spec (rounding mode, mask) the operand may be KIND_NONE: the
     * default is encoded; for a hidden spec the operand is KIND_NONE (the field
     * is copied)
     */
    static bool encode(const riscvdis::RiscvOperandSpec& spec,
                       const RiscvDecodedOperand& operand,
                       const riscvdis::RiscvCodecContext& context,
                       uint32& encoding);

    /*
     * Return true if a spec is a mnemonic part (RISCV_KIND_AQRL) and not an
     * operand
     */
    static bool isMnemonicPart(const riscvdis::RiscvOperandSpec& spec);

    /*
     * Return true if the encoding passes the constraints of the vector
     * registers which LLVM checks when it assembles (the RISCV_VR_* flags of
     * the destination spec of the entry): the destination of a masked
     * instruction isn't v0, the destination doesn't overlap vs2 / vs1. The
     * decoder never calls this: LLVM decodes the reserved overlaps. The
     * assembler calls it for the bits of the entry it encoded
     */
    static bool checkAssemblerConstraints(const riscvdis::RiscvEncodingEntry& entry,
                                          uint32 encoding);

    /*
     * Return true if a spec never has text (RISCV_KIND_GPR_TIED). The decoder
     * skips it, the assembler calls encode() with an empty operand for it
     */
    static bool isHidden(const riscvdis::RiscvOperandSpec& spec);

    /*
     * Return true if an operand is optional (RISCV_KIND_VMASK,
     * RISCV_KIND_ROUNDING_MODE: the last operands, which the text may leave
     * out), and if so whether it prints for these bits (a vm of 0, a
     * rounding mode which isn't the default). The decoder doesn't add the
     * operand when it doesn't print; the assembler encodes the default (vm
     * = 1, the default rounding mode) when the text leaves it out
     */
    static bool isOptional(const riscvdis::RiscvOperandSpec& spec);
    static bool isPrinted(const riscvdis::RiscvOperandSpec& spec, uint32 encoding);

    /*
     * Return the text of a mnemonic part for the bits (".aqrl"), "" if it
     * prints nothing
     */
    static cString decodeMnemonicPart(const riscvdis::RiscvOperandSpec& spec,
                                      uint32 encoding);

    /*
     * Encode a mnemonic part from the text which follows the base mnemonic:
     * consume its text from the start of 'suffix' and set its field
     *
     * Return false if the text doesn't start with a valid value of the part
     * (an empty value is valid: no aq / rl)
     */
    static bool encodeMnemonicPart(const riscvdis::RiscvOperandSpec& spec,
                                   cString& suffix,
                                   uint32& encoding);

    /*
     * Expand a compressed instruction into the 32 bit instruction of the
     * specification (the tables of the "RVC instruction set listings": c.addi
     * rd, imm is addi rd, rd, imm; c.lwsp rd, imm is lw rd, imm(sp)...);
     * c.unimp is the csrrw x0, cycle, x0 which LLVM prints as unimp, c.mop.1
     * and c.mop.5 are sspush x1 and sspopchk x5, the other c.mop have no
     * expansion
     *
     * type     - The instruction set (c.jal is RV32, c.addiw RV64)
     * encoding - The 16 bit instruction
     * expanded - Will be filled with the 32 bit instruction
     *
     * Return false if the instruction is reserved (no expansion)
     */
    static bool expandCompressed(RiscvInstructionSet::DisassemblerTypes type,
                                 uint16 encoding,
                                 uint32& expanded);

    /*
     * Find a CSR by its name / return the printed name of a CSR
     *
     * Return false (NULL) if the name / the number is unknown or its feature
     * isn't in the set
     */
    static bool findCsr(const char* name,
                        const riscvdis::RiscvCodecContext& context,
                        uint& number);
    static const char* getCsrName(uint number,
                                  const riscvdis::RiscvCodecContext& context);

    /*
     * Return the bits of a field / set the bits of a field
     */
    static uint32 getField(uint32 encoding, uint position, uint width);
    static void setField(uint32& encoding, uint position, uint width, uint32 value);
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVOPERANDCODEC_H
