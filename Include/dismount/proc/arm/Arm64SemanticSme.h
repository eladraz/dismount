#ifndef __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSME_H
#define __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSME_H

/*
 * Arm64SemanticSme.h
 *
 * The parts of the A64 lifter of the SME and SME2 instructions
 * (gArm64SmeEncodingTable) and the helpers they share. Not a public header:
 * the lifter is ArmSemanticLifter (ArmSemantics.h).
 *
 *   Arm64SemanticLiftSme.cpp        the dispatcher, the helpers, the moves
 *                                   between za and the vectors, the loads and
 *                                   stores of za, zero, zt0
 *   Arm64SemanticLiftSmeOuter.cpp   the outer products, addha / addva
 *   Arm64SemanticLiftSmeArray.cpp   the instructions which accumulate into
 *                                   the vectors of the za array (add, fmla,
 *                                   the dot products, the long multiply adds)
 *   Arm64SemanticLiftSmeVector.cpp  the multi vector instructions of the
 *                                   z registers (min, max, clamp, shifts,
 *                                   zip, unpack, the conversions, luti)
 *
 * The model: the vectors are SVL bits (the streaming vector length of the
 * description, Arm64Architecture), za is SVL / 8 rows (registers
 * ARM64_REG_ZA_ROW0...) of SVL bits, the horizontal slices of the za.b tile.
 * The tile t of an element size of E bytes has the rows t, t + E, t + 2 E...;
 * its vertical slices are the elements of its rows. A slice or array vector
 * select is computed at runtime from the 32 bit register of the instruction.
 *
 * A streaming instruction traps (OP_TRAP, TRAP_UNDEFINED) when SVCR.SM is
 * clear, and an instruction which uses za or zt0 when SVCR.ZA is clear. The
 * checks are branches over the body (SmeLift::begin / end).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/proc/arm/Arm64SemanticSimd.h"

namespace armdis {

/*
 * The lifter of the mnemonics of gArm64SmeEncodingTable. Every entry of
 * gArm64SmeSemanticTable is this function.
 */
void liftArm64Sme(ArmLiftContext& context);

/*
 * smstart / smstop / msr svcrsm, #imm (A64 PSTATE field, Arm64SemanticLift.cpp): 'crm' is the CRm
 * field (bit 0: start, bits 3..1: 1 streaming mode, 2 za, 3 both)
 */
void liftSmeModeChange(ArmLiftContext& context, uint crm);
/*
 * msr svcr, x<n>: the SM and ZA bits of the 64 bit 'value' are written as smstart / smstop do
 * (a bit which doesn't change has no effect), mrs svcr reads the register
 */
void liftSmeWriteSvcr(ArmLiftContext& context, const SemanticOperand& value);

class SmeLift;

// Arm64SemanticLiftSme.cpp: mova, movaz, movt, ld1 / st1 of slices, ldr / str of
// za and zt0, zero. Each group lifter returns true if it handled the
// instruction
bool liftSmeMove(SmeLift& m);
// Arm64SemanticLiftSmeOuter.cpp: fmopa / fmops, bfmopa, smopa..., bmopa, addha, addva
bool liftSmeOuter(SmeLift& m);
// Arm64SemanticLiftSmeArray.cpp: the accumulation into the za array vectors
bool liftSmeArray(SmeLift& m);
// Arm64SemanticLiftSmeVector.cpp: the multi vector z instructions
bool liftSmeVector(SmeLift& m);
// Arm64SemanticLiftSmeConvert.cpp: zip, uzp, unpack, the conversions, the saturating
// narrows, the rounding to integral, luti
bool liftSmeConvert(SmeLift& m);

/*
 * The decoded za operand of an instruction: a tile (za0.s), a slice
 * (za0h.s[w12, 1]) or an array vector (za.s[w8, 1, vgx2])
 */
struct SmeZa {
    // The element size in bits (8 to 128)
    uint m_elementBits;
    // The tile number
    uint m_tile;
    // 0: a tile or an array, 1: horizontal slice, 2: vertical slice
    uint m_direction;
    // True for an array operand (za.s[...])
    bool m_isArray;
    // The register id of the slice / vector select (w12...), the first and
    // the last offset, the vector group (0, 2 or 4)
    uint m_selectRegister;
    uint m_offset;
    uint m_offsetLast;
    uint m_group;
};

/*
 * The helpers of the SME lifters
 */
class SmeLift {
public:
    typedef SemanticOperand Val;

    explicit SmeLift(ArmLiftContext& context);

    ArmLiftContext& c() { return m_context; }
    SimdLift& s() { return m_simd; }
    // The mnemonic of the encoding entry ("fmopa", "mova")
    const char* name() const { return m_simd.name(); }
    bool is(const char* name) const { return m_simd.is(name); }
    uint32 encoding() const { return m_simd.encoding(); }
    uint count() const { return m_context.getOperandsCount(); }
    const ArmDecodedOperand& operand(uint index) const
    {
        return m_context.getOperand(index);
    }

    // The element size of the arrangement of register operand 'index' (8 to 64),
    // 0 if none
    uint elementBits(uint index) const;
    // The number of registers of a z register or list operand, the number of
    // the 'r'th z register, and the number of a single z / p register operand
    uint groupCount(uint index) const;
    uint groupRegister(uint index, uint r) const;
    uint zNumber(uint index) const { return groupRegister(index, 0); }
    uint pNumber(uint index) const;

    // The streaming vector length in bits (the size of za rows and of the
    // z and p registers in the streaming mode), the number of rows of za
    uint svl() const { return m_svl; }
    uint rows() const { return m_svl / 8; }
    // The number of elements of 'elementBits' bits in a vector
    uint lanes(uint elementBits) const { return m_svl / elementBits; }

    //////////////////////////////////////////////////////////////////////
    // The mode checks: begin() checks SVCR (streaming: SM, za: ZA) and
    // branches to the trap, end() places the trap. Every lifter calls them
    //////////////////////////////////////////////////////////////////////
    void begin(bool isStreaming, bool isZa);
    void end();

    //////////////////////////////////////////////////////////////////////
    // Types and registers
    //////////////////////////////////////////////////////////////////////
    SemanticType vecType(uint elementBits) const;
    SemanticType floatVecType(uint elementBits) const;
    // The 'index'th z register / predicate as a value of SVL bits
    Val z(uint number);
    // z<number> as a vector of 'elementBits' lanes
    Val zv(uint number, uint elementBits);
    // Write z<number> (the value has SVL bits, or is a vector of them)
    void setZ(uint number, const Val& value);
    // The predicate as a mask of the lanes of 'elementBits': lane i is the
    // bit i * elementBits / 8 of the predicate
    Val laneMask(uint predicateNumber, uint elementBits);
    // The mask 'a & b'
    Val andMask(const Val& a, const Val& b);
    // A value reinterpreted as the vector of 'elementBits' lanes / as an
    // integer of SVL bits
    Val asVec(const Val& value, uint elementBits);
    Val asBits(const Val& value);
    // The lane 'index' as an integer, a vector with the lane replaced
    Val lane(const Val& vector, uint index);
    Val setLane(const Val& vector, const Val& scalar, uint index);
    // A vector of 'lanes' lanes of 'elementBits' bits with every lane the
    // scalar
    Val splat(const Val& scalar, uint elementBits);
    Val zeroVec(uint elementBits);
    // The 32 bit register 'registerId' (w0...)
    Val wRegister(uint registerId);
    // An integer constant
    Val imm(int64 value, uint bits);

    //////////////////////////////////////////////////////////////////////
    // za
    //////////////////////////////////////////////////////////////////////
    // Decode the KIND_MATRIX operand 'index'
    SmeZa za(uint index) const;
    // The row 'row' of za (a constant): a vector of SVL bits
    Val zaRow(uint row);
    void setZaRow(uint row, const Val& value);
    // The row given by a runtime value (an integer of 32 bits)
    Val zaRowAt(const Val& row);
    void setZaRowAt(const Val& row, const Val& value);
    // The runtime slice index (w + offset + extra) mod (the slice dimension)
    Val sliceIndex(const SmeZa& z, uint extra, uint groupRound);
    // The array vector select: (w + offset) mod (rows / group), the first
    // of the vectors
    Val arraySelect(const SmeZa& z, uint plus, uint groupRound);
    // The row of the horizontal slice 'index' of the tile
    Val tileRowOfSlice(const SmeZa& z, const Val& sliceIndexValue);
    // A whole slice of the tile as a vector of the element size
    Val readSlice(const SmeZa& z, const Val& sliceIndexValue);
    void writeSlice(const SmeZa& z, const Val& sliceIndexValue,
                    const Val& vector);
    // The row 'row' (a constant) of the tile as a vector of its elements
    Val tileRow(const SmeZa& z, uint row);
    void setTileRow(const SmeZa& z, uint row, const Val& vector);

    //////////////////////////////////////////////////////////////////////
    // Floating point: the operations on ZA accumulate with the default NaN
    // and without the exception flags (isZa), the others as the guest
    // control register
    //////////////////////////////////////////////////////////////////////
    Val fp(SemanticOperation::Operation operation, uint elementBits,
           const Val& a, const Val& b, const Val& c, bool isZa,
           SemanticOperation::Behavior behavior = SemanticOperation::BEHAVIOR_DEFAULT,
           bool isBFloat = false);
    // The lanes k, k + step, k + 2 step... of a vector, as a vector of lanes / step
    // lanes
    Val deinterleave(const Val& vector, uint k, uint step = 2);
    // The vector (of SVL bits) as lanes of 'groupBits' bits where lane i is the
    // lane 'index' of the 128 bit segment of i (the indexed element of the by
    // element instructions)
    Val indexedVector(const Val& vector, uint groupBits, uint index);
    // Extend the lanes of a vector to 'toBits' bits
    Val extendLanes(const Val& vector, uint toBits, bool isSigned);
    // A mask of the lanes (an integer of one bit per lane) reduced to the bits
    // k, k + 2, k + 4...: an integer with half the bits
    Val deinterleaveBits(const Val& bits, uint k);
    // Bit 'index' of an integer as a 1 bit value
    Val bitOf(const Val& bits, uint index);
    // The mask as an integer of one bit per lane, and back
    Val maskToBits(const Val& mask);
    Val bitsToMask(const Val& bits);
    // A mask 'condition ? mask : none'
    Val gateMask(const Val& condition, const Val& mask);
    // A vector of half precision numbers whose denormals are flushed to zero
    // (the sign is kept) when FPCR.FZ16 is set: the widening instructions
    // multiply the half precision inputs, which the flush applies to
    Val flushHalf(const Val& vector);
    // The sign bit of every element flipped (also scalar)
    Val flipSign(const Val& value, uint elementBits);
    // A conversion between the float formats (vectors of the same lanes)
    Val fpConvert(const Val& a, SemanticType::FloatFormat from,
                  SemanticType::FloatFormat to, uint lanesCount, bool isZa);

private:
    ArmLiftContext& m_context;
    SimdLift m_simd;
    uint m_svl;
    uint m_registerBits;
    SemanticOperand m_trapLabel;
    SemanticOperand m_endLabel;
    bool m_hasChecks;
};

} // namespace armdis

#endif // __TBA_DISMOUNT_PROC_ARM_ARM64SEMANTICSME_H
