#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICVECTOR_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICVECTOR_H

/*
 * RiscvSemanticVector.h
 *
 * The infrastructure of the vector (V) lifters of RISC-V
 * (RiscvSemanticLiftVector*.cpp) and the helpers they share. Not a public
 * header: the lifter is RiscvSemanticLifter (RiscvSemantics.h).
 *
 * The model
 * ---------
 * vtype, vl and vstart are registers read at runtime: the same instruction
 * runs with any SEW and LMUL. VLEN is known when the instruction is lifted
 * (the RiscvArchitecture). So the IR of a vector instruction is
 *
 *     snapshot vl and vstart
 *     trap (illegal instruction) if vtype.vill is set
 *     dispatch on (vtype.vsew, vtype.vlmul): the 28 legal combinations, each
 *         with its own body, in which SEW, LMUL, EEW, EMUL, the number of
 *         registers of a group, the legality of the register numbers and
 *         the widths of the vector types are lift-time constants (a body
 *         emits OP_TRAP for a combination the instruction reserves)
 *     vstart = 0
 *
 * RvvLift::dispatch() emits the frame and calls the body once for each
 * combination; a body is a function (RvvBody) of the combination. An
 * optimizer which knows vtype keeps one body and removes the others.
 *
 * The elements vstart .. vl - 1 are the ones the instruction works on; the
 * ones of a masked instruction that v0 deselects, the tail (>= vl) and the
 * prestart (< vstart) elements stay as they were (both policies are
 * undisturbed; the agnostic ones allow it, and the reference processors are
 * configured so, see tests/riscv/semantics/TEST_PLAN.md). Mask registers use
 * the layout MLEN = 1: element i is bit i.
 *
 * Two engines:
 *
 *   1. The chunks (RvvLift::elementwise()): the instructions that work on
 *      each element on its own (arithmetic, compares, conversions...). The
 *      operands are register groups; the engine cuts the elements into
 *      chunks of C = VLEN / (the widest EEW of the operands) lanes, so a
 *      chunk of any operand lives inside one register, and the body of the
 *      instruction is written once per chunk on whole vectors of C lanes
 *      (a callback, RvvElementOp, from the lane vectors of the operands to
 *      the lane vector of the result). The engine reads the operands
 *      (extending the narrow ones of a widening instruction), builds the
 *      active lanes (vstart, vl and v0), merges the result with the old
 *      value of the destination, and writes the destination after every
 *      chunk is computed (so the overlaps the architecture allows read the
 *      old values). This is the way to add an arithmetic instruction: see
 *      "Extension points".
 *   2. The element loops (RvvLift::beginLoop() ...): the instructions with
 *      a data dependent order or address (the loads and stores, vrgather,
 *      the slides, vcompress, viota): a loop over the elements whose
 *      counter is vstart itself, so a fault leaves the architectural
 *      vstart. A register of a group is accessed by OP_READ_REGISTER_INDEXED
 *      and OP_WRITE_REGISTER_INDEXED over v0..v31 with the register number
 *      computed from the element index at runtime.
 *
 * The checks of the reserved encodings follow the specification (and what
 * Spike checks): a register group aligned to its EMUL, EMUL within 1/8..8,
 * the overlaps of the widening and narrowing instructions, v0 as a
 * destination of a masked instruction, vstart != 0 for the instructions
 * that require 0 (reductions, vcpop, vfirst, vmsbf, viota, vcompress...; the
 * arithmetic ones honor vstart).
 *
 * Extension points (how the floating point, BFloat16, Zvbb / Zvkb / Zvbc and the
 * vector cryptography use the infrastructure: RiscvSemanticLiftVectorFloat.cpp,
 * ...Bits.cpp and ...Crypto.cpp are the examples)
 * ----------------------------------------------------------------
 *   - An arithmetic instruction (also the floating point ones, Zvbb: vandn,
 *     vrol, vror, vwsll, vbrev8..., Zvbc: vclmul): add a row to the table of
 *     an element lifter, give it an RvvPlan (shape, extension of the narrow
 *     operands, vm handling) and an RvvElementOp. The callback gets the
 *     operands as integer vectors of the compute lane width
 *     (RvvChunk::m_a = vs2, m_b = vs1 / the scalar broadcast, m_old = the
 *     old vd) and bit casts them to float vectors itself. The scalar operand
 *     of the OPFVF encodings (funct3 = 5) is read by RvvPlan::m_readScalar
 *     (the NaN boxed f register of the format); the OPFVV ones (funct3 = 1)
 *     read the vector like OPIVV. For the widening floating point
 *     instructions set RvvPlan::m_extendA / m_extendB to EXTEND_NONE: the callback then
 *     gets the narrow lanes (C lanes of the narrow width) and converts them
 *     (OP_FLOAT_CONVERT) itself.
 *   - A sketch of vfadd.vv / vfadd.vf (SEW 16, 32, 64: the body of the
 *     dispatch returns trapIllegal() for the other SEWs; the real one is
 *     floatElementOp()):
 *         SemanticOperand floatAdd(RvvChunk& c)
 *         {
 *             const SemanticType t = SemanticType::vector(
 *                 SemanticType::floating(formatOf(c.m_bits)), c.m_lanes);
 *             SemanticInstruction add(SemanticOperation::OP_FLOAT_ADD, t, result,
 *                 c.m_lift.bitcast(c.m_a, t), c.m_lift.bitcast(c.m_b, t));
 *             add.setAttributes(ATTRIBUTE_FLOAT_EXCEPTIONS);
 *             add.setRoundingMode(SemanticOperation::ROUND_DYNAMIC);
 *             ... emit it; return c.m_lift.bitcast(result, c.m_vectorType);
 *         }
 *         body: RvvPlan plan; plan.m_op = floatAdd; plan.m_sanitizeA = 0;
 *               plan.m_sanitizeB = 0; plan.m_readScalar = readBoxedFloat;
 *               lift.elementwise(plan, type);
 *   - The floating point flags and the rounding mode: every OP_FLOAT_*
 *     instruction carries SemanticInstruction::ATTRIBUTE_FLOAT_EXCEPTIONS
 *     and the rounding mode, and the interpreter accumulates the exceptions
 *     of every lane into fflags. The vector floating point instructions have
 *     no rm field, so RiscvLiftContext::getRoundingMode(0) gives
 *     ROUND_DYNAMIC (frm) and emits the check of the reserved values of frm
 *     (an illegal instruction): call it once in the body, before the chunks.
 *     A lane that is not active (inactive, tail, prestart) must not raise
 *     flags: RvvChunk::sanitize() replaces the inactive lanes of an operand
 *     by a value that raises nothing (0, or the identity of the operation);
 *     the engine does it for vs2 / vs1 when RvvPlan::m_sanitizeA / B is
 *     the neutral value (not negative; call sanitize() from the callback for
 *     an all ones neutral value).
 *   - The permutations with a floating point twin are the bodies of
 *     RiscvSemanticLiftVectorPermute.cpp with the scalar from an f register
 *     (parameter bit 8), the floating point reductions are
 *     RiscvSemanticLiftVectorFloat.cpp (element loops, in element order, for
 *     the sums: vfredusum is the ordered sum too, a choice the specification
 *     allows). Originally planned as: vfmv.f.s / vfmv.s.f are moveScalarBody (vmv.x.s / vmv.s.x) with the
 *     NaN boxing of RiscvLiftContext::readFloat / writeFloat, vfslide1up /
 *     vfslide1down are slideBody with the scalar from the f register,
 *     vfredusum / vfredmax / vfredmin are reduceBody with an OP_FLOAT_*
 *     combine (OP_REDUCE; the order of the unordered sum is its Behavior) and
 *     vfredosum / vfwredosum an element loop (beginLoop) in element order with
 *     the sequential accumulation (BEHAVIOR_REDUCE_ORDERED); vfmerge.vfm and
 *     vfmv.v.f are the RVV_OP_MERGE / RVV_OP_MOVE plans with the f scalar
 *     (RvvPlan::m_readScalar).
 *   - The vm bit and the mask: RvvPlan::m_mask. Instructions which use
 *     v0 as data (vmerge, vadc: MASK_DATA) get the carry as RvvChunk::
 *     m_carry.
 *   - The element groups of the cryptography (EGS = 4 or 8 elements of
 *     SEW 32 / 64, vl / EGS groups, vstart in units of groups): they
 *     don't fit the lane engine. RiscvSemanticLiftVectorCrypto.cpp unrolls the
 *     groups of the register group at lift time (a group of 256 bits spans two
 *     registers at VLEN = 128, the EGW must fit LMUL * VLEN), computes them as
 *     integers of EGW bits, and writes them after all are computed.
 *   - The rows of the semantic table: RiscvSemanticTableVector.cpp includes
 *     one .inc of plain rows per area (the generated one of this half and
 *     RiscvSemanticTable_vector_float.inc, ..._bits.inc, ..._crypto.inc). The
 *     row macros of that file (RVA for a chunk engine arithmetic row with
 *     RVV_PARAM(), RVP, RVM, RVL, RVS, RVX / RVR for the floating point, RVB
 *     and RVK) take the mnemonic and the parameter of the lifter.
 *   - vsetvl{i}: vtype bit 8 (altfmt of Zvfbfa) and the other upper bits
 *     are reserved here (vill), see RiscvSemanticLiftVectorConfig.cpp
 *     (RvvLift::vtypeIsValid()).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/riscv/RiscvSemantics.h"
#include "dismount/proc/riscv/RiscvSemanticInternal.h"

namespace riscvdis {

/*
 * A (SEW, LMUL) combination of vtype, known when a body is emitted
 */
struct RvvType {
    // 8, 16, 32 or 64
    uint m_sew;
    // log2(LMUL): -3 (1/8) to 3 (8)
    int m_lmulLog2;
    // The value vtype[5:0] has for the combination
    uint m_key;

    // The number of registers of a group of LMUL (1 for a fractional one)
    uint registers() const;
    // The number of elements of the group (VLMAX) for VLEN bits registers
    uint vlmax(uint vlen) const;
};

/*
 * A register group operand: the first register, the element width and
 * log2(EMUL) (-3 .. 3)
 */
struct RvvGroup {
    uint m_register;
    uint m_eew;
    int m_emulLog2;

    // The number of registers of the group
    uint registers() const;
};

class RvvLift;
class RvvChunk;

/*
 * The body of an instruction for a (SEW, LMUL) combination
 */
typedef void (*RvvBody)(RvvLift& lift, const RvvType& type);

/*
 * The element operation of the chunk engine: compute the result lane
 * vector (of chunk.m_vectorType, or the mask of the lanes for a mask
 * destination) from the operands of the chunk
 */
typedef SemanticOperand (*RvvElementOp)(RvvChunk& chunk);

/*
 * The reader of the scalar operand of an instruction whose funct3 isn't one
 * of the integer ones (the OPFVF f register): the scalar of 'bits' bits
 */
typedef SemanticOperand (*RvvScalarReader)(RvvLift& lift, uint bits);

/*
 * How the chunk engine runs an instruction
 */
struct RvvPlan {
    enum Shape {
        // vd, vs2, vs1 / the scalar all at SEW
        SHAPE_SAME,
        // vd at 2 * SEW; vs2 and vs1 at SEW (vwadd.vv, vwadd.vx)
        SHAPE_WIDEN,
        // vd and vs2 at 2 * SEW; vs1 at SEW (vwadd.wv)
        SHAPE_WIDEN_W,
        // vd at SEW; vs2 at 2 * SEW; vs1 at SEW (vnsrl.wv, vnclip)
        SHAPE_NARROW,
        // vd at SEW; vs2 at SEW / m_factor (vzext.vf2)
        SHAPE_EXTEND,
        // vd is a mask register; vs2 and vs1 at SEW (vmseq, vmadc)
        SHAPE_MASK_DEST
    };
    // How the narrow operands of a widening instruction reach the callback
    enum Extend {
        // Zero extended / sign extended to the compute width
        EXTEND_ZERO,
        EXTEND_SIGN,
        // As they are (C lanes of the narrow width): the callback converts
        EXTEND_NONE
    };
    // The meaning of the vm bit
    enum MaskMode {
        // vm = 0 masks the elements by v0 (the destination can't be v0)
        MASK_BY_V0,
        // The instruction has no mask: v0 is data (vadc, vmerge, vmadc:
        // m_carry), only the tail and prestart are inactive
        MASK_DATA,
        // The encoding has no vm bit worth a mask (vmv.v.x...): the same as
        // MASK_DATA without the v0 operand
        MASK_NONE
    };
    Shape m_shape;
    // The element width ratio of SHAPE_EXTEND: 2, 4 or 8
    uint m_factor;
    // The extension of vs2 / of vs1 (the scalar too) in a widening
    // instruction
    Extend m_extendA;
    Extend m_extendB;
    // The vi immediate is unsigned (uimm5), else signed (simm5)
    bool m_isImmediateUnsigned;
    MaskMode m_mask;
    // The destination is also a source (vmacc, vwmacc): the callback gets it
    // as chunk.m_old
    bool m_readsDestination;
    // The callback sets chunk.m_saturated: vxsat |= any active lane
    bool m_setsSaturation;
    // The instruction has no vs1 / scalar operand (vzext, vmv.v.v of vs2...)
    bool m_hasNoB;
    // Replace the inactive lanes of vs2 / vs1 by this value before the
    // callback (the floating point instructions: no flags for them); -1 for
    // no replacement
    int64 m_sanitizeA;
    int64 m_sanitizeB;
    // The reader of funct3 = 1 / 5 scalar operands, or NULL
    RvvScalarReader m_readScalar;
    // The callback and its arguments (an SemanticOperation::Operation or a
    // private id, and an integer)
    RvvElementOp m_op;
    uint m_operation;
    int64 m_argument;

    // The defaults: SHAPE_SAME, signed vi, MASK_BY_V0
    RvvPlan();
};

/*
 * The state of the engine for one chunk, given to the callback
 */
class RvvChunk {
public:
    RvvChunk(RvvLift& lift, const RvvType& type, const RvvPlan& plan);

    RvvLift& m_lift;
    const RvvType& m_type;
    const RvvPlan& m_plan;
    // The chunk number, the number of lanes and the index of its first
    // element
    uint m_index;
    uint m_lanes;
    uint m_firstElement;
    // The width of the lanes of the operands as the callback gets them (the
    // compute width: the width of the destination, or of the wide operand)
    uint m_bits;
    // The types: the integer vector of the compute width, the mask of the
    // lanes
    SemanticType m_vectorType;
    SemanticType m_maskType;
    // The operands (integer vectors of m_vectorType; the narrow ones of an
    // EXTEND_NONE plan are vectors of the narrow width): vs2, vs1 or the
    // scalar broadcast, the old value of vd
    SemanticOperand m_a;
    SemanticOperand m_b;
    SemanticOperand m_old;
    // The scalar operand of a vx / vi / vf instruction (XLEN bits for the
    // integer ones, before the conversion to the lane width), if any
    SemanticOperand m_scalar;
    // The active lanes (a mask) and, for MASK_DATA, the carry / select mask
    // (v0's bits of the chunk; invalid for the others)
    SemanticOperand m_active;
    SemanticOperand m_carry;
    // Set by the callback: the lanes that saturated (a mask), if the plan
    // m_setsSaturation
    SemanticOperand m_saturated;

    // Replace the lanes of 'value' that are not active by 'neutral'
    SemanticOperand sanitize(const SemanticOperand& value, int64 neutral);
};

/*
 * The helper of the vector lifters of one instruction
 */
class RvvLift {
public:
    explicit RvvLift(RiscvLiftContext& context);

    RiscvLiftContext& context() { return m_context; }
    uint xlen() const { return m_xlen; }
    uint vlen() const { return m_vlen; }

    /*
     * The instruction
     */
    uint32 encoding() const;
    uint field(uint position, uint width) const;
    uint vd() const { return field(7, 5); }
    uint vs1() const { return field(15, 5); }
    uint vs2() const { return field(20, 5); }
    // The rs1 field (a number 0-31, an x register, a v register or the
    // immediate of the OPIVI forms)
    uint rs1() const { return field(15, 5); }
    uint funct3() const { return field(12, 3); }
    uint funct6() const { return field(26, 6); }
    // true if vm = 0: the instruction is masked by v0
    bool isMasked() const { return field(25, 1) == 0; }
    // The simm5 / uimm5 immediate of the OPIVI forms
    int64 simm5() const;
    uint uimm5() const { return field(15, 5); }

    /*
     * Types and values
     */
    SemanticType integerType(uint bits) const;
    SemanticType vectorType(uint elementBits, uint lanes) const;
    SemanticType maskType(uint lanes) const;
    SemanticType xlenType() const;
    SemanticOperand imm(int64 value, const SemanticType& type) const;
    SemanticOperand immX(int64 value) const;

    /*
     * Emit an operation, with a behavior, and the common composites. The
     * type is the type of the operation (the vector type for lane
     * operations)
     */
    SemanticOperand emit(SemanticOperation::Operation operation,
                         const SemanticType& type,
                         const SemanticOperand& a = SemanticOperand(),
                         const SemanticOperand& b = SemanticOperand(),
                         const SemanticOperand& c = SemanticOperand());
    SemanticOperand emitBehavior(SemanticOperation::Operation operation,
                                 const SemanticType& type,
                                 const SemanticOperand& a,
                                 const SemanticOperand& b,
                                 SemanticOperation::Behavior behavior);
    void move(const SemanticOperand& destination, const SemanticOperand& source);
    // Lane compare: the mask (for vectors) or a boolean (for scalars)
    SemanticOperand compare(SemanticOperation::Condition condition,
                            const SemanticType& type,
                            const SemanticOperand& a,
                            const SemanticOperand& b);
    // mask ? a : b (vectors), or a boolean ? a : b
    SemanticOperand select(const SemanticOperand& condition,
                           const SemanticOperand& a,
                           const SemanticOperand& b);
    SemanticOperand bitcast(const SemanticOperand& value, const SemanticType& type);
    // Integer scalar conversions to 'bits'
    SemanticOperand zeroExtend(const SemanticOperand& value, uint bits);
    SemanticOperand signExtend(const SemanticOperand& value, uint bits);
    SemanticOperand truncate(const SemanticOperand& value, uint bits);
    // Lane conversions of a vector to elements of 'bits' (same lanes)
    SemanticOperand extendLanes(const SemanticOperand& value, uint bits,
                                bool isSigned);
    SemanticOperand truncateLanes(const SemanticOperand& value, uint bits);
    // The scalar 'value' (an integer) in every lane of the vector type
    SemanticOperand broadcast(const SemanticOperand& value,
                              const SemanticType& vectorType);
    // The vector type with every lane 'value'
    SemanticOperand splat(int64 value, const SemanticType& vectorType);
    // bits [offset, offset + type bits) of an integer value, and the value
    // with them replaced
    SemanticOperand extractBits(const SemanticOperand& value,
                                const SemanticOperand& offset,
                                const SemanticType& type);
    SemanticOperand insertBits(const SemanticOperand& value,
                               const SemanticOperand& piece,
                               const SemanticOperand& offset);
    // A mask of 'lanes' lanes with all the lanes set / the lanes l with
    // first + l < limit (OP_ACTIVE_LANE_MASK)
    SemanticOperand allLanes(uint lanes);
    SemanticOperand lanesBelow(uint lanes, const SemanticOperand& first,
                               const SemanticOperand& limit);
    // A boolean ? all lanes : no lanes
    SemanticOperand conditionMask(const SemanticOperand& condition, uint lanes);
    // Add an unsigned 1 bit condition: 'a != 0'
    SemanticOperand isNonZero(const SemanticOperand& value);

    /*
     * Registers
     */
    // The vector register v<number> as an integer of VLEN bits (a copy, so
    // later writes of the register don't change it)
    SemanticOperand readRegister(uint number);
    // A write of the whole register
    void writeRegister(uint number, const SemanticOperand& value);
    // The register v<number> for a runtime 'number' (an XLEN integer; the
    // register file is v0 - v31, modulo 32), and the write
    SemanticOperand readRegisterIndexed(const SemanticOperand& number);
    void writeRegisterIndexed(const SemanticOperand& number,
                              const SemanticOperand& value);
    // The x register 'number' (x0 reads 0), XLEN bits
    SemanticOperand readX(uint number);
    void writeX(uint number, const SemanticOperand& value);
    // The x register as a scalar operand of 'bits' bits: truncated when XLEN
    // is wider, sign extended when it is narrower (the .vx / .vf operands)
    SemanticOperand readScalar(uint number, uint bits);
    // The scalar of the .vf instructions: the f register rs1 as an integer of
    // 'bits' bits (16, 32 or 64). A value which is not NaN boxed (the bits
    // above 'bits' are not all ones) reads as 'canonicalNan', the canonical
    // NaN of its format
    SemanticOperand readFloatScalar(uint bits, uint64 canonicalNan);
    // f<number> = value, an integer of 'bits' bits, NaN boxed
    void writeFloatRegister(uint number, const SemanticOperand& value);
    // vl, vstart, vtype as the snapshot taken by dispatch() / readState()
    const SemanticOperand& vl() const { return m_vl; }
    const SemanticOperand& vstart() const { return m_vstart; }
    // The snapshot of the registers at this point
    void readState();
    // The register operands (not snapshots)
    SemanticOperand vlRegister();
    SemanticOperand vstartRegister();
    SemanticOperand vtypeRegister();

    /*
     * Traps and labels
     */
    void trapIllegal();
    // Trap if the boolean 'condition' is set
    void trapIllegalIf(const SemanticOperand& condition);
    SemanticOperand newLabel();
    void placeLabel(const SemanticOperand& label);
    void branch(const SemanticOperand& label);
    void branchIf(const SemanticOperand& condition, const SemanticOperand& label);

    /*
     * The frame
     *
     * dispatch(): snapshot vl and vstart, trap if vill, and call 'body' for
     * each (SEW, LMUL) combination, the others (the reserved combinations)
     * trap. The caller adds finish() after.
     * requireValid(): only the vill check (instructions which don't depend
     * on SEW and LMUL: the mask logical instructions, vcpop.m, vmv.x.s...).
     * finish(): vstart = 0.
     */
    void dispatch(RvvBody body);
    void requireValid();
    void finish();
    // The first part of dispatch (the snapshot and the vill check) alone,
    // for the instructions that ignore vill (whole register moves)
    void begin(bool checkVill);

    /*
     * The register groups. A body checks them at lift time and emits
     * trapIllegal() when one is reserved.
     */
    // true if the group is a legal register group: EMUL in 1/8 .. 8 and the
    // first register aligned to EMUL
    static bool isGroupLegal(const RvvGroup& group);
    // true if the register ranges [a, a + a regs) and [b, ...) overlap
    static bool isOverlapped(const RvvGroup& a, const RvvGroup& b);
    // The overlap of a wider destination with a narrower source that is
    // legal: the source in the highest numbered part of the destination,
    // with the source EMUL at least 1 (a is the destination)
    static bool isOverlappedWiden(const RvvGroup& a, const RvvGroup& b);
    // The group of an operand at 'eew' / the EMUL following from the EEW
    // ratio (EEW / SEW * LMUL); m_emulLog2 is out of -3 .. 3 when illegal
    static RvvGroup makeGroup(const RvvType& type, uint registerNumber,
                              uint eew);
    static RvvGroup makeGroupLmul(const RvvType& type, uint registerNumber,
                                  uint eew, int emulLog2);
    // Element groups of the cryptography (EGS elements of EEW bits each:
    // EGW bits): the group fits a register of VLEN bits
    bool isElementGroupLegal(uint egw) const;

    /*
     * The chunk engine: run 'plan' for the instruction at (SEW, LMUL) 'type'.
     * Emits the checks of the register groups too
     */
    void elementwise(const RvvPlan& plan, const RvvType& type);

    /*
     * Element access in a register group at a lift-time constant index,
     * and pieces of a group
     */
    // The piece 'chunk' of a group: 'lanes' lanes of the group's EEW starting
    // at element chunk * lanes, as an integer vector
    SemanticOperand readPiece(const RvvGroup& group, uint chunk, uint lanes);
    void writePiece(const RvvGroup& group, uint chunk, uint lanes,
                    const SemanticOperand& value);
    // The registers of a group (up to 8) as VLEN bit integers
    void readGroup(const RvvGroup& group, SemanticOperand* registers);
    void writeGroup(const RvvGroup& group, const SemanticOperand* registers);

    /*
     * Elements at a runtime index of a register group: element 'index' (an
     * XLEN integer) of the group starting at 'registerNumber' with 'eew' bit
     * elements, as an integer of 'eew' bits, and the write
     */
    SemanticOperand readElement(uint registerNumber, uint eew,
                                const SemanticOperand& index);
    void writeElement(uint registerNumber, uint eew,
                      const SemanticOperand& index,
                      const SemanticOperand& value);
    // The register offset and the bit offset in the register of an element
    SemanticOperand elementRegister(uint eew, const SemanticOperand& index);
    SemanticOperand elementOffset(uint eew, const SemanticOperand& index);
    // Bit 'index' of the register v0 (the mask bit of an element), a boolean
    SemanticOperand maskBit(const SemanticOperand& index);
    // Bit 'index' of register 'number' as a boolean, and the write of it
    SemanticOperand readMaskBit(uint number, const SemanticOperand& index);
    void writeMaskBit(uint number, const SemanticOperand& index,
                      const SemanticOperand& bit);

    /*
     * The element loop. beginLoop(limit) emits
     *     loop: i = vstart; if (i >= limit) goto done
     * and returns i (a temporary valid inside the body); the body skips an
     * element with skipElement(); endLoop() emits vstart = i + 1, the jump
     * and the label done, and vstart is 0 at the end of the instruction
     * (finish()).
     */
    SemanticOperand beginLoop(const SemanticOperand& limit);
    // Jump to the end of the iteration (the element is skipped), always or if
    // the boolean is set
    void skipElement();
    void skipElementIf(const SemanticOperand& condition);
    // Leave the loop (a fault only first load trimming vl)
    void leaveLoop();
    void endLoop();

    /*
     * Fixed point rounding (vxrm). The increment to add to a value that
     * was shifted right by 'shift' (a lane vector of the compute width: the
     * number of bits shifted out, 0 .. bits - 1): 'value' is the original,
     * 'shifted' the result of the shift. The mode comes from vxrm at
     * runtime: rnu, rne, rdn, rod.
     */
    SemanticOperand roundingIncrement(RvvChunk& chunk,
                                      const SemanticOperand& value,
                                      const SemanticOperand& shift,
                                      const SemanticOperand& shifted);
    // The runtime vxrm as a 2 bit scalar
    SemanticOperand readRoundingMode();
    // Record that lanes saturated: vxsat |= 1 if any lane of the mask
    void setSaturation(const SemanticOperand& anyBoolean);

    /*
     * vtype helpers (RiscvSemanticLiftVectorConfig.cpp): the validity of a
     * vtype value and its VLMAX, as IR on runtime values
     */
    // A boolean: the vtype value is a legal configuration
    SemanticOperand vtypeIsValid(const SemanticOperand& vtype);
    // VLMAX of a legal vtype value (XLEN bits)
    SemanticOperand vtypeVlmax(const SemanticOperand& vtype);

    // The end label of the dispatch (a body may leave early)
    SemanticOperand endLabel() const { return m_endLabel; }

private:
    RiscvLiftContext& m_context;
    uint m_xlen;
    uint m_vlen;
    SemanticOperand m_vl;
    SemanticOperand m_vstart;
    SemanticOperand m_endLabel;
    // The element loop in progress
    SemanticOperand m_loopLabel;
    SemanticOperand m_nextLabel;
    SemanticOperand m_doneLabel;
    SemanticOperand m_loopIndex;
    // The saturation accumulator of the body in progress
    SemanticOperand m_saturation;
};

/*
 * The element operations of the arithmetic lifters (the first field of the
 * parameter of the table entries, RVV_PARAM()). The ones from
 * RVV_OP_FIXED_FIRST on are in RiscvSemanticLiftVectorFixed.cpp
 */
enum RvvArithmeticOp {
    RVV_OP_ADD, RVV_OP_SUB, RVV_OP_RSUB,
    RVV_OP_MINU, RVV_OP_MIN, RVV_OP_MAXU, RVV_OP_MAX,
    RVV_OP_AND, RVV_OP_OR, RVV_OP_XOR,
    RVV_OP_SLL, RVV_OP_SRL, RVV_OP_SRA,
    RVV_OP_MUL, RVV_OP_MULH, RVV_OP_MULHU, RVV_OP_MULHSU,
    RVV_OP_DIVU, RVV_OP_DIV, RVV_OP_REMU, RVV_OP_REM,
    // The carries: vadc, vsbc (the sum / the difference), vmadc, vmsbc (the
    // carry / the borrow out, a mask), vmerge, vmv.v.*
    RVV_OP_ADC, RVV_OP_SBC, RVV_OP_MADC, RVV_OP_MSBC,
    RVV_OP_MERGE, RVV_OP_MOVE,
    // The compares (masks): ==, !=, <u, <, <=u, <=, >u, >
    RVV_OP_SEQ, RVV_OP_SNE, RVV_OP_SLTU, RVV_OP_SLT, RVV_OP_SLEU,
    RVV_OP_SLE, RVV_OP_SGTU, RVV_OP_SGT,
    // vd = vs1 * vs2 + vd, vd - vs1 * vs2, vs1 * vd + vs2, vs2 - vs1 * vd
    RVV_OP_MACC, RVV_OP_NMSAC, RVV_OP_MADD, RVV_OP_NMSUB,
    // The narrowing shifts, and vzext / vsext (the extension is the plan's)
    RVV_OP_NSRL, RVV_OP_NSRA, RVV_OP_EXTEND, RVV_OP_ID,
    // Fixed point
    RVV_OP_FIXED_FIRST,
    RVV_OP_SADDU = RVV_OP_FIXED_FIRST, RVV_OP_SADD, RVV_OP_SSUBU, RVV_OP_SSUB,
    RVV_OP_AADDU, RVV_OP_AADD, RVV_OP_ASUBU, RVV_OP_ASUB,
    RVV_OP_SMUL, RVV_OP_SSRL, RVV_OP_SSRA, RVV_OP_NCLIPU, RVV_OP_NCLIP
};

/*
 * The parameter of an arithmetic entry: the operation, the shape (RvvPlan::
 * Shape), the extension of vs2 and of vs1 / the scalar (RvvPlan::Extend), the
 * mask mode (RvvPlan::MaskMode) and the flags
 */
enum {
    RVV_FLAG_IMMEDIATE_UNSIGNED = 1,
    RVV_FLAG_READS_DESTINATION = 2,
    RVV_FLAG_SATURATES = 4,
    RVV_FLAG_NO_B = 8
};
#define RVV_PARAM(operation, shape, extendA, extendB, mask, flags, factor) \
    ((uint64)(operation) | ((uint64)(shape) << 8) | \
     ((uint64)(extendA) << 12) | ((uint64)(extendB) << 14) | \
     ((uint64)(mask) << 16) | ((uint64)(flags) << 20) | \
     ((uint64)(factor) << 28))

// Build the plan of an arithmetic entry from its parameter; the element
// operation is the one of the integer or the fixed point file
RvvPlan makeArithmeticPlan(uint64 parameter);
// The element operations (RiscvSemanticLiftVectorInteger.cpp, ...Fixed.cpp)
SemanticOperand integerElementOp(RvvChunk& chunk);
SemanticOperand fixedElementOp(RvvChunk& chunk);
SemanticOperand floatElementOp(RvvChunk& chunk);
SemanticOperand bitsElementOp(RvvChunk& chunk);

/*
 * The lifters of the vector mnemonics (the m_lift of the entries of
 * gRiscvVectorSemanticTable)
 */
// vsetvli, vsetivli, vsetvl (RiscvSemanticLiftVectorConfig.cpp); the
// parameter is 0, 1, 2
void liftVsetvl(RiscvLiftContext& context);
// The loads and stores (RiscvSemanticLiftVectorMemory.cpp), the parameter is
// built by RVV_MEMORY()
void liftVectorMemory(RiscvLiftContext& context);
// The integer and fixed point arithmetic (RiscvSemanticLiftVectorInteger.cpp,
// ...Fixed.cpp): the parameter is built by RVV_PARAM()
void liftVectorInteger(RiscvLiftContext& context);
/*
 * The vector cryptography (RiscvSemanticLiftVectorCrypto.cpp): the parameter of
 * the entries is the operation
 */
enum RvvCryptoOp {
    RVK_AESEF_VV, RVK_AESEF_VS, RVK_AESEM_VV, RVK_AESEM_VS, RVK_AESDF_VV,
    RVK_AESDF_VS, RVK_AESDM_VV, RVK_AESDM_VS, RVK_AESZ_VS, RVK_AESKF1,
    RVK_AESKF2, RVK_GHSH, RVK_GMUL, RVK_SHA2MS, RVK_SHA2CH, RVK_SHA2CL,
    RVK_SM3ME, RVK_SM3C, RVK_SM4K, RVK_SM4R_VV, RVK_SM4R_VS
};
void liftVectorCrypto(RiscvLiftContext& context);

/*
 * The vector bit manipulation of Zvbb, Zvkb and Zvbc
 * (RiscvSemanticLiftVectorBits.cpp): the operations of the entries, whose
 * parameter is built by RVV_PARAM()
 */
enum RvvBitsOp {
    RVB_OP_ANDN, RVB_OP_BREV, RVB_OP_BREV8, RVB_OP_REV8, RVB_OP_CLZ,
    RVB_OP_CTZ, RVB_OP_CPOP, RVB_OP_ROL, RVB_OP_ROR, RVB_OP_WSLL,
    RVB_OP_CLMUL, RVB_OP_CLMULH
};
void liftVectorBits(RiscvLiftContext& context);

/*
 * The vector floating point (RiscvSemanticLiftVectorFloat.cpp): the element
 * operations of the chunk engine, the parameter of the entries is built by
 * RVF_PARAM(): the operation, the shape (RvvPlan::Shape), the mask mode
 * (RvvPlan::MaskMode) and the flags
 */
enum RvvFloatOp {
    RVF_OP_ADD, RVF_OP_SUB, RVF_OP_RSUB, RVF_OP_MUL, RVF_OP_DIV, RVF_OP_RDIV,
    // vfmin, vfmax (the minimumNumber / maximumNumber of IEEE 754-2019) and
    // the sign injection
    RVF_OP_MIN, RVF_OP_MAX, RVF_OP_SGNJ, RVF_OP_SGNJN, RVF_OP_SGNJX,
    // The multiply-add family (the flags tell the member), the square root,
    // the estimates and the class
    RVF_OP_MACC, RVF_OP_SQRT, RVF_OP_REC7, RVF_OP_RSQRT7, RVF_OP_CLASS,
    RVF_OP_MERGE, RVF_OP_MOVE,
    // The compares into a mask: ==, !=, <, <=, >, >=
    RVF_OP_EQ, RVF_OP_NE, RVF_OP_LT, RVF_OP_LE, RVF_OP_GT, RVF_OP_GE,
    // The conversions: float to integer, integer to float, float to float
    RVF_OP_TO_INTEGER, RVF_OP_FROM_INTEGER, RVF_OP_CONVERT
};
enum {
    // The integer of the conversion is unsigned
    RVF_FLAG_UNSIGNED = 1,
    // The conversion rounds toward zero / to odd (static modes)
    RVF_FLAG_RTZ = 2,
    RVF_FLAG_ROD = 4,
    // The 16 bit float of the instruction is BFloat16 (Zvfbfmin, Zvfbfwma)
    RVF_FLAG_BFLOAT16 = 8,
    // The instruction doesn't depend on frm: a reserved value of it is not
    // an illegal instruction
    RVF_FLAG_NO_FRM = 16,
    // The multiply-add family: the product / the addend is negated, and
    // the multiplicand is vd (vfmadd) instead of vs2 (vfmacc)
    RVF_FLAG_NEGATE_PRODUCT = 32,
    RVF_FLAG_NEGATE_ADDEND = 64,
    RVF_FLAG_MULTIPLY_DESTINATION = 128
};
#define RVF_PARAM(operation, shape, mask, flags) \
    ((uint64)(operation) | ((uint64)(shape) << 8) | ((uint64)(mask) << 12) | \
     ((uint64)(flags) << 16))
// The vector floating point instructions; the reductions
// (RiscvSemanticLiftVectorFloat.cpp): the parameter is RVF_REDUCE_* |
// (isWide << 8)
void liftVectorFloat(RiscvLiftContext& context);
enum { RVF_REDUCE_SUM_ORDERED, RVF_REDUCE_SUM_UNORDERED, RVF_REDUCE_MIN,
       RVF_REDUCE_MAX };
void liftVectorFloatReduce(RiscvLiftContext& context);

/*
 * The parameter of a memory entry: the kind (RVV_MEM_*), store, fault only
 * first, the number of fields - 1 and log2(element width / 8). For an indexed
 * access the width is the one of the index
 */
enum {
    RVV_MEM_UNIT, RVV_MEM_STRIDED, RVV_MEM_INDEXED_ORDERED,
    RVV_MEM_INDEXED_UNORDERED, RVV_MEM_WHOLE, RVV_MEM_MASK
};
#define RVV_MEMORY(kind, isStore, isFaultFirst, fields, eewLog2) \
    ((uint64)(kind) | ((uint64)(isStore) << 4) | \
     ((uint64)(isFaultFirst) << 5) | ((uint64)((fields) - 1) << 8) | \
     ((uint64)(eewLog2) << 12))

// The mask instructions (RiscvSemanticLiftVectorMask.cpp); the parameter is
// a RvvMaskOp
enum RvvMaskOp {
    RVV_MASK_AND, RVV_MASK_NAND, RVV_MASK_ANDN, RVV_MASK_XOR, RVV_MASK_OR,
    RVV_MASK_NOR, RVV_MASK_ORN, RVV_MASK_XNOR,
    RVV_MASK_CPOP, RVV_MASK_FIRST, RVV_MASK_SBF, RVV_MASK_SOF, RVV_MASK_SIF,
    RVV_MASK_IOTA
};
void liftVectorMask(RiscvLiftContext& context);
// Permutation and reductions (RiscvSemanticLiftVectorPermute.cpp); the
// parameter is a RvvPermuteOp
enum RvvPermuteOp {
    RVV_PERMUTE_MV_X_S, RVV_PERMUTE_MV_S_X,
    RVV_PERMUTE_SLIDEUP, RVV_PERMUTE_SLIDEDOWN, RVV_PERMUTE_SLIDE1UP,
    RVV_PERMUTE_SLIDE1DOWN,
    RVV_PERMUTE_GATHER, RVV_PERMUTE_GATHER_EI16, RVV_PERMUTE_COMPRESS,
    // The whole register moves; the number of registers is the field
    // (RVV_PERMUTE_MOVE | (registers << 8))
    RVV_PERMUTE_MOVE,
    // (vfmv.f.s, vfmv.s.f, vfslide1up.vf and vfslide1down.vf are the
    // RVV_PERMUTE_MV_X_S, MV_S_X, SLIDE1UP and SLIDE1DOWN with the scalar in
    // an f register: | (1 << 8))
    // The reductions: RVV_PERMUTE_REDUCE | (RVV_REDUCE_* << 8) | (isWide <<
    // 16) | (isSigned << 17)
    RVV_PERMUTE_REDUCE
};
enum { RVV_REDUCE_SUM, RVV_REDUCE_AND, RVV_REDUCE_OR, RVV_REDUCE_XOR,
       RVV_REDUCE_MIN, RVV_REDUCE_MAX };
void liftVectorPermute(RiscvLiftContext& context);

} // namespace riscvdis

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVSEMANTICVECTOR_H
