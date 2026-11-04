#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVENCODINGTABLE_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVENCODINGTABLE_H

/*
 * RiscvEncodingTable.h
 *
 * The encoding tables of the RISC-V instruction sets (RV32 and RV64, the
 * compressed instructions, the privileged instructions and the vector
 * extensions). The disassembler, the formatter, the assembler and the
 * semantic lifter are all driven by them, as the ARM ones are
 * (ArmEncodingTable.h).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/riscv/RiscvFeatures.h"

namespace riscvdis {

/*
 * How an operand is encoded: the kind of the operand and where its fields
 * are (RiscvOperandSpec). RiscvOperandCodec (RiscvOperandCodec.h) turns a
 * spec and the instruction bits into a RiscvDecodedOperand, and a
 * RiscvDecodedOperand back into the bits. The text is never produced from the
 * bits: the formatter (RiscvNotation) prints the RiscvDecodedOperand and the
 * assembler parses the text into one, so decoding, formatting, parsing and
 * encoding are inverse functions which the tests check against each other
 * (tests/riscv/TEST_PLAN.md).
 *
 * Naming of the kinds:
 *     RISCV_KIND_<what>[_<variant>]
 * To add a kind: add it here with its m_position / m_flags / m_extra
 * meaning, then to the decode and encode switches of RiscvOperandCodec*.cpp.
 * The formatter and the parser don't change unless the kind produces a new
 * RiscvDecodedOperand::Kind.
 */
enum RiscvOperandKind {
    // No operand (the end of the list)
    RISCV_KIND_NONE,

    // ---- Parts of the mnemonic -------------------------------------------
    // These kinds are formatted into the mnemonic, in their order in the
    // entry, not as operands.
    //
    // The aq and rl bits of the atomics, bit 26 (aq) and 25 (rl) at
    // m_position 25: "", ".aq", ".rl", ".aqrl" ("amoadd.w.aqrl", "lr.w.aq")
    RISCV_KIND_AQRL,

    // ---- Integer registers ------------------------------------------------
    // x0-x31: the register number, 5 bits at m_position (rd 7, rs1 15, rs2
    // 20, rs3 27). m_flags are RiscvGprFlags
    RISCV_KIND_GPR,
    // The compressed register x8-x15: 3 bits at m_position (rd' / rs1' /
    // rs2'), the register is 8 + the field
    RISCV_KIND_GPR_C,
    // A register which isn't in the bits but is printed: m_extra is the
    // register number (sp of c.addi16sp / c.addi4spn / c.lwsp in the syntax
    // of the compressed instructions, ra of c.sspush)
    RISCV_KIND_GPR_FIXED,
    // A register field which must be equal to another one and is not printed
    // (rd = rs1 of c.addi / c.add, the rs1 of the zero register of the
    // aliases): m_position is the field, m_extra the position of the field
    // it repeats. The decoder checks the equality (else the entry doesn't
    // match), the assembler copies the field
    RISCV_KIND_GPR_TIED,

    // ---- Floating point and vector registers ------------------------------
    // f0-f31: 5 bits at m_position
    RISCV_KIND_FPR,
    // The compressed f8-f15: 3 bits at m_position
    RISCV_KIND_FPR_C,
    // v0-v31: 5 bits at m_position (vd 7, vs1 15, vs2 20, vs3 7). The
    // register group of LMUL > 1 is the same operand: the alignment of the
    // group depends on vtype and is checked at runtime, not by the decoder.
    // m_flags RISCV_VR_* (RiscvOperandCodec.h) are the constraints which
    // LLVM checks when it assembles (the destination of a widening
    // instruction overlapping a source, a masked destination of v0); they
    // are set on the destination (vd) spec. m_extra is the register class
    // of the operand: bits 0-2 the log2 of the alignment (vmv2r.v, vl4re8.v
    // and vs8r.v name the first register of a group of 2, 4 or 8 registers:
    // the encoding of an unaligned register is invalid), bits 3-6 the number
    // of registers of a segment tuple (vlseg3e8.v: 3), which only the
    // overlap constraints of the assembler use. 0 for a single register
    RISCV_KIND_VR,
    // The fixed v0 of the carry / merge instructions (vadc.vvm, vmerge.vvm,
    // vfmerge.vfm): printed "v0", no bits
    RISCV_KIND_VR_V0,
    // The mask: vm, 1 bit at m_position (25). 0 prints the operand ", v0.t",
    // 1 prints nothing (the operand is optional, the last one). The
    // instructions without a masked form have the bit fixed in m_mask
    RISCV_KIND_VMASK,

    // ---- Immediates -------------------------------------------------------
    // An integer immediate: m_extra is a RiscvImmediateScheme (which bits,
    // the sign, the scale: I, S, U, CI, CIW..., the vector simm5 / uimm5,
    // the CSR uimm5, the lpad label), m_flags RiscvImmediateFlags (non zero,
    // printed as hex...)
    RISCV_KIND_IMMEDIATE,
    // A shift amount: the field at m_position (bit 20, bit 2 of the
    // compressed shifts), m_extra the width: 6 for the shifts of XLEN
    // (shamt[5] must be 0 in RV32, else the encoding is reserved), 5 for
    // the w shifts of RV64 and the 32 bit operations of Zbkb / Zkn
    RISCV_KIND_SHAMT,
    // The floating point immediate of fli (Zfa): the 5 bit index at
    // m_position (15) into the table of the values of the Zfa specification
    // (RiscvOperandCodec.h: gRiscvFliValues), printed as LLVM does ("min",
    // "inf", "nan", "1.0", "0.5", "1.0e-07"...)
    RISCV_KIND_FLOAT_IMMEDIATE,

    // ---- Memory -------------------------------------------------------------
    // "offset(base)": the base register at m_position (5 bits, or 3 bits
    // with RISCV_MEMORY_COMPRESSED, or sp with RISCV_MEMORY_SP), the offset
    // from the RiscvImmediateScheme m_extra; RISCV_MEMORY_NO_OFFSET is the
    // "(base)" of the atomics, lr / sc, cbo.* and the vector loads. m_flags
    // are RiscvMemoryFlags
    RISCV_KIND_MEMORY,

    // ---- Control flow -------------------------------------------------------
    // A pc relative target: the offset from the RiscvImmediateScheme m_extra
    // (RISCV_IMM_B of the branches, RISCV_IMM_J of jal, RISCV_IMM_CB /
    // RISCV_IMM_CJ of the compressed ones), added to the address of the
    // instruction
    RISCV_KIND_BRANCH_TARGET,

    // ---- System -------------------------------------------------------------
    // A CSR: 12 bits at m_position (20), printed by its name when the name
    // is known and its feature is in the set (the names of LLVM: "fflags",
    // "cycle", "vl", "stimecmp", "seed"), as a number otherwise
    RISCV_KIND_CSR,
    // The rounding mode of the floating point instructions: 3 bits at
    // m_position (12). An optional last operand: LLVM omits "dyn" (7) and
    // prints the static modes ("rne", "rtz", "rdn", "rup", "rmm"); the
    // conversions which can't round (fcvt.d.s, fcvt.d.w...) print it only
    // when it isn't the default of LLVM for them, see m_flags
    // RiscvRoundingFlags. 5 and 6 are reserved: LLVM decodes them as
    // invalid
    RISCV_KIND_ROUNDING_MODE,
    // The predecessor / successor set of fence: 4 bits at m_position (24 /
    // 20), "i", "o", "r", "w" letters ("iorw", "rw"), "0" for none
    RISCV_KIND_FENCE_SET,
    // The vtype immediate of vsetvli (11 bits at 20) / vsetivli (10 bits at
    // 20): m_extra is the width. "e32, m1, ta, ma" (sew, lmul, the tail and
    // mask policies); the reserved values (vill, the reserved sew / lmul,
    // the high bits) are printed as the number, as LLVM does
    RISCV_KIND_VTYPE
};

// The flags of RISCV_KIND_GPR / RISCV_KIND_GPR_C
enum RiscvGprFlags {
    // x0 is reserved for this entry (c.addi rd = 0 is a hint, c.lui rd = 0
    // is reserved...): the entry doesn't match it
    RISCV_GPR_NOT_ZERO      = 0x01,
    // x2 is reserved for this entry (c.lui: rd = 2 is c.addi16sp)
    RISCV_GPR_NOT_SP        = 0x02,
    // An even register: the first of a pair (amocas.d of RV32, amocas.q),
    // printed as the first register; an odd one is reserved
    RISCV_GPR_EVEN          = 0x04
};

/*
 * An optional constraint of an encoding, which its mask and value can't
 * give (a field which must differ from another one, a reserved combination).
 * Return true if 'encoding' is this entry
 */
typedef bool (*RiscvEncodingCondition)(uint32 encoding);

/*
 * The spec of one operand of an encoding, see RiscvOperandKind
 */
struct RiscvOperandSpec {
    // A RiscvOperandKind
    uint8 m_kind;
    // The bit of the main field (the low bit)
    uint8 m_position;
    // Per kind, see the kind
    uint8 m_flags;
    // Per kind: a scheme number, a width, a register number...
    uint8 m_extra;
};

// The maximum number of operand specs of an encoding (mnemonic parts
// included): fmadd.s fd, fs1, fs2, fs3, rm has 5; vmacc.vv vd, vs1, vs2, v0.t
// has 4
enum { RISCV_MAX_OPERAND_SPECS = 6 };

// The flags of the entries
enum RiscvEntryFlags {
    // The entry is an alias of a more general encoding (li of addi, mv,
    // ret of jalr, nop, j, beqz, csrr, fmv.s of fsgnj.s, vmv.v.v of
    // vmerge...): the decoder finds it first since it comes first in the
    // table. It is skipped by the formatter of RISCV_FORMAT_NO_ALIASES (the
    // text of objdump -M no-aliases). The assembler accepts both
    RISCV_ENTRY_ALIAS           = 0x0001,
    // The assembler doesn't produce this encoding (a second encoding of the
    // same text)
    RISCV_ENTRY_DECODE_ONLY     = 0x0002,
    // Only the assembler uses the entry: LLVM reads the text but never
    // prints it
    RISCV_ENTRY_ASSEMBLE_ONLY   = 0x0004,
    // The encoding exists in RV32 only / in RV64 only
    RISCV_ENTRY_RV32_ONLY       = 0x0008,
    RISCV_ENTRY_RV64_ONLY       = 0x0010,
    // The encoding is a HINT of the specification (the integer instructions
    // whose destination is x0, c.nop with an immediate, c.addi with an
    // immediate of 0...): LLVM decodes it as the instruction, the lifter
    // lifts it as the instruction (which has no effect). A compressed hint
    // has no expansion: LLVM prints it by its compressed name (c.li zero, 3,
    // c.nop 3, c.ntl.p1)
    RISCV_ENTRY_HINT            = 0x0020,
    // A privileged instruction (mret, sret, wfi, sfence.vma, hlv...): the
    // lifter lifts it to a helper (RiscvSemantics.h) or a trap
    RISCV_ENTRY_PRIVILEGED      = 0x0040,
    // The instruction ends a basic block for the lifter of a translator even
    // without changing the flow (fence.i: the code may have changed)
    RISCV_ENTRY_SERIALIZING     = 0x0080,
    // An alias which only the expansion of a compressed instruction is printed
    // by: LLVM prints c.mv as "mv a0, a1" but the 32 bit "add a0, zero, a1"
    // as it is. The decoder skips the entry for the 32 bit instructions
    RISCV_ENTRY_EXPANSION_ALIAS = 0x0100,
    // The decoder ignores m_feature (not m_feature2): LLVM's disassembler
    // decodes the encoding (Zicsr, Zifencei, the hint aliases ntl.*, pause,
    // lpad, prefetch.*, and sspush / sspopchk / ssrdp with Zimop only...)
    // whichever features are enabled. The assembler still checks both
    // (llvm-mc rejects the instruction without its feature)
    RISCV_ENTRY_DECODE_UNGATED  = 0x0200
};

/*
 * An encoding of an instruction.
 *
 * The encoding is a 32 bit value: the 32 bit instruction, or the 16 bit
 * compressed one in the low 16 bits (the tables of the compressed
 * instructions only).
 *
 * An encoding matches when (bits & m_mask) == m_value, its feature (and
 * m_feature2) are in the target's RiscvFeatureSet, it exists in the
 * instruction set (RISCV_ENTRY_RV32_ONLY / RV64_ONLY) and m_condition (if
 * any) returns true. The first matching entry of a table wins, so an alias
 * comes before the general encoding, and a specific encoding before an
 * overlapping general one. The decoder may index a table by some of its bits
 * (the major opcode, bits 0-6) for speed, but must keep this order.
 *
 * The mnemonic is the LLVM one, lowercase, without the parts which come from
 * fields (the aq / rl suffix, RISCV_KIND_AQRL): "add", "lw", "fadd.s",
 * "amoadd.w", "vadd.vv", "vle32.v", "c.addi". The compressed instructions
 * have their "c." name, the text of objdump -M no-aliases; the default
 * formatter prints the 32 bit instruction they expand to (see
 * RiscvNotation), as objdump does.
 */
struct RiscvEncodingEntry {
    // The mnemonic, or NULL for the end of the table
    const char* m_mnemonic;
    // The fixed bits. m_value is also the start of the assembler: the bits
    // of m_value outside m_mask are the canonical value of the fields that
    // the decoder ignores, which the assembler writes
    uint32 m_mask;
    uint32 m_value;
    // The operands and the mnemonic parts, in the order of the syntax,
    // ended by RISCV_KIND_NONE
    RiscvOperandSpec m_operands[RISCV_MAX_OPERAND_SPECS];
    // The RiscvFeatures::Feature which adds the encoding
    uint16 m_feature;
    // RiscvEntryFlags
    uint16 m_flags;
    // The Opcode::FlowAlter of the instruction (FLOW_NO_ALTER for most)
    uint16 m_flow;
    // An extra constraint, or NULL
    RiscvEncodingCondition m_condition;
    // A second feature which the encoding needs too (c.fld: Zcd is the
    // feature, nothing else; fli.h: Zfa with Zfh; amocas.b: Zabha with
    // Zacas; c.mul: Zcb with Zmmul), FEAT_I (always on) for none. The tables
    // leave it out
    uint16 m_feature2;
};

/*
 * The tables, each ended by an entry whose mnemonic is NULL. The
 * disassembler searches the tables of the size of the instruction in this
 * order: the compressed table for the 16 bit instructions, the base, the
 * vector (V, memory, extensions) and the privileged tables for the 32 bit ones.
 */
// The 32 bit unprivileged scalar instructions: RV32I / RV64I, M, A, F, D,
// Zicsr, Zifencei, Zfh, Zfa, Zfbfmin, the bit manipulation and the scalar
// cryptography, Zicond, Zimop, Zawrs, Zacas, Zabha, Zicbo*, the hints,
// Zicfilp, Zicfiss
extern const RiscvEncodingEntry gRiscvBaseEncodingTable[];
// The 16 bit compressed instructions: Zca, Zcf, Zcd, Zcb, Zcmop and the
// compressed Zicfiss
extern const RiscvEncodingEntry gRiscvCompressedEncodingTable[];
// The vector instructions: V (the Zve subsets): the configuration, the
// integer, fixed point, floating point, mask, permutation and reduction
// instructions
extern const RiscvEncodingEntry gRiscvVectorEncodingTable[];
// The vector loads and stores: the unit stride, strided, indexed, segment,
// fault only first, whole register and mask ones
extern const RiscvEncodingEntry gRiscvVectorMemoryEncodingTable[];
// The vector extensions: Zvfbfmin, Zvfbfwma, Zvbb, Zvkb, Zvbc and the vector
// cryptography (Zvkg, Zvkned, Zvknha, Zvknhb, Zvksed, Zvksh)
extern const RiscvEncodingEntry gRiscvVectorExtensionEncodingTable[];
// The privileged instructions: mret, sret, wfi, sfence.vma, Svinval, H
extern const RiscvEncodingEntry gRiscvPrivilegedEncodingTable[];

} // namespace riscvdis

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVENCODINGTABLE_H
